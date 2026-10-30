#include "backend/frames.hpp"

#include "backend/frame_share.hpp"
#include "backend/irq.hpp"

#include <algorithm>

namespace tisple {

namespace {

// the runtime and single-sequence scratch: scratch(2) rt_a(2) rt_b(2) rt_r(2) rt_s(2)
constexpr std::uint32_t kScratchBytes = 10;
// small globals that may go to zero page
constexpr std::uint32_t kMaxZpGlobal = 16;

struct Item {
    std::int32_t sym = -1;
    std::int32_t var = -1;
    std::int32_t function = -1; // -1: a global
    std::uint32_t size = 0;
    bool forced = false;
    bool zp = false;
    std::uint64_t weight = 0;
    std::uint32_t offset = 0; // inside its frame part
    std::int32_t slot = -1;   // FrameShares' slot: every var in it gets these bytes
    // a variable of the program no code names: bytes only the interpreter uses
    bool phantom = false;
};

struct Stack {
    const Program* program = nullptr;
    std::vector<std::int32_t> order;
    std::vector<std::vector<std::int32_t>> callers;
};

// size of the overlapped stack for these frame sizes; offsets per function
std::uint32_t StackSize(const Stack& st, const std::vector<std::uint32_t>& sizes,
                        std::vector<std::uint32_t>* offsets) {
    std::vector<std::uint32_t> off(sizes.size(), 0);
    std::uint32_t total = 0;
    for (std::int32_t f : st.order) {
        std::uint32_t start = 0;
        for (std::int32_t c : st.callers[static_cast<std::size_t>(f)]) {
            start = std::max(start,
                             off[static_cast<std::size_t>(c)] + sizes[static_cast<std::size_t>(c)]);
        }
        off[static_cast<std::size_t>(f)] = start;
        total = std::max(total, start + sizes[static_cast<std::size_t>(f)]);
    }
    if (offsets) {
        *offsets = off;
    }
    return total;
}

// the longest run of free zero-page bytes
void LongestRun(const std::vector<bool>& free, std::uint32_t& start, std::uint32_t& length) {
    start = 0;
    length = 0;
    std::uint32_t run = 0;
    for (std::uint32_t a = 0; a <= 256; a++) {
        if (a < 256 && free[a]) {
            run++;
            continue;
        }
        if (run > length) {
            length = run;
            start = a - run;
        }
        run = 0;
    }
}

void AddVarItem(Backend& be, std::vector<Item>& items, std::int32_t var, std::int32_t function) {
    std::uint32_t size = be.shapes.vars[static_cast<std::size_t>(var)].size;
    if (size == 0) {
        return;
    }
    Item item;
    item.var = var;
    item.sym = be.var_sym[static_cast<std::size_t>(var)];
    item.function = function;
    item.size = size;
    item.forced = PointerInZp(be, var);
    item.weight = be.info[static_cast<std::size_t>(item.sym)].weight;
    items.push_back(item);
}

// the interrupt handler's functions run between any two instructions of everything else:
// their frames start after every other function's (the handler counts every one of those as
// a caller), and come last in the order
void HandlerFramesApart(const Program& program, Stack& st) {
    std::vector<bool> handler = HandlerFunctions(program);
    std::vector<std::int32_t> rest;
    std::vector<std::int32_t> irq;
    for (std::int32_t f : st.order) {
        (handler[static_cast<std::size_t>(f)] ? irq : rest).push_back(f);
    }
    st.order = rest;
    st.order.insert(st.order.end(), irq.begin(), irq.end());
    for (std::int32_t f : rest) {
        st.callers[static_cast<std::size_t>(program.handler)].push_back(f);
    }
}

// offsets inside each function's zero-page and memory frame parts: the program's variables
// first, then what only the code uses (temporaries, made locals); then over the latter the
// variables no code names, which only the interpreter gives values (it needs them apart from
// the program's other variables, and the code never reaches them), in zero page while they
// fit over it, else in memory; zp_used and mem_used get each part's size
void PlaceInFrames(const Program& program, std::vector<Item>& items,
                   std::vector<std::uint32_t>& zp_used, std::vector<std::uint32_t>& mem_used) {
    auto own = [&](const Item& item) {
        return item.var >= 0 && item.slot < 0 &&
               !program.vars[static_cast<std::size_t>(item.var)].made;
    };
    for (int pass = 0; pass < 2; pass++) {
        for (Item& item : items) {
            if (item.function < 0 || item.phantom || own(item) != (pass == 0)) {
                continue;
            }
            std::size_t f = static_cast<std::size_t>(item.function);
            std::uint32_t& used = item.zp ? zp_used[f] : mem_used[f];
            item.offset = used;
            used += item.size;
        }
    }
    std::size_t n = zp_used.size();
    std::vector<std::uint32_t> zp_own(n, 0);
    std::vector<std::uint32_t> mem_own(n, 0);
    for (const Item& item : items) {
        if (item.function >= 0 && !item.phantom && own(item)) {
            std::size_t f = static_cast<std::size_t>(item.function);
            (item.zp ? zp_own[f] : mem_own[f]) += item.size;
        }
    }
    std::vector<std::uint32_t> zp_phantoms(n, 0);
    std::vector<std::uint32_t> mem_phantoms(n, 0);
    for (Item& item : items) {
        if (item.function < 0 || !item.phantom) {
            continue;
        }
        std::size_t f = static_cast<std::size_t>(item.function);
        item.zp = zp_phantoms[f] + item.size <= zp_used[f] - zp_own[f];
        std::uint32_t& used = item.zp ? zp_phantoms[f] : mem_phantoms[f];
        item.offset = (item.zp ? zp_own[f] : mem_own[f]) + used;
        used += item.size;
    }
    for (std::size_t f = 0; f < n; f++) {
        zp_used[f] = std::max(zp_used[f], zp_own[f] + zp_phantoms[f]);
        mem_used[f] = std::max(mem_used[f], mem_own[f] + mem_phantoms[f]);
    }
}

} // namespace

std::vector<std::int32_t> CallersFirst(const Program& program) {
    std::size_t n = program.functions.size();
    std::vector<std::uint32_t> callers(n, 0);
    for (const Function& fn : program.functions) {
        for (std::int32_t callee : fn.callees) {
            callers[static_cast<std::size_t>(callee)]++;
        }
    }
    std::vector<std::int32_t> order;
    for (std::size_t f = 0; f < n; f++) {
        if (callers[f] == 0) {
            order.push_back(static_cast<std::int32_t>(f));
        }
    }
    for (std::size_t i = 0; i < order.size(); i++) {
        for (std::int32_t callee : program.functions[static_cast<std::size_t>(order[i])].callees) {
            if (--callers[static_cast<std::size_t>(callee)] == 0) {
                order.push_back(callee);
            }
        }
    }
    return order;
}

bool AllocateFrames(Backend& be, FramePlan& plan) {
    const Program& program = *be.program;
    std::size_t fn_count = program.functions.size();
    Stack st;
    st.program = &program;
    st.order = CallersFirst(program);
    st.callers.resize(fn_count);
    for (std::size_t f = 0; f < fn_count; f++) {
        for (std::int32_t callee : program.functions[f].callees) {
            st.callers[static_cast<std::size_t>(callee)].push_back(static_cast<std::int32_t>(f));
        }
    }
    if (st.order.size() != fn_count) {
        AddError(*be.errors, Loc(), "internal: the call graph has a cycle");
        return false;
    }
    if (program.handler >= 0) {
        HandlerFramesApart(program, st);
    }

    // zero page left after explicit placements
    std::vector<bool> free(256, true);
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const Var& var = program.vars[v];
        if (!var.placement.has_address || var.placement.address >= 0x100) {
            continue;
        }
        std::uint32_t end = var.placement.address + be.shapes.vars[v].size;
        for (std::uint32_t a = var.placement.address; a < end && a < 256; a++) {
            free[a] = false;
        }
    }
    if (program.handler >= 0) {
        // on every IRQ the II+ and IIe roms save A there
        if (!free[kIrqSaveA]) {
            AddError(*be.errors, Loc(),
                     "an :address let takes zero page $45, where the rom saves A on every "
                     "interrupt; move it");
            return false;
        }
        free[kIrqSaveA] = false;
    }
    std::uint32_t run = 0;
    std::uint32_t length = 0;
    LongestRun(free, run, length);
    if (length < kScratchBytes) {
        AddError(*be.errors, Loc(),
                 "explicit :address placements leave no 10 free zero-page bytes in a row for "
                 "the runtime; move one");
        return false;
    }
    std::int32_t scratch[5] = {be.scratch, be.rt_a, be.rt_b, be.rt_r, be.rt_s};
    for (int i = 0; i < 5; i++) {
        DefineSymbol(be.as, scratch[i], run + 2 * static_cast<std::uint32_t>(i));
        be.as.symbols[static_cast<std::size_t>(scratch[i])].zp = true;
    }
    run += kScratchBytes;
    length -= kScratchBytes;

    // the items that compete for the rest; made locals share slots (frame_share.hpp)
    FrameShares shares;
    ShareMadeLocals(be, st.callers, shares);
    std::vector<Item> items;
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const Var& var = program.vars[v];
        const VarPlace& shape = be.shapes.vars[v];
        bool small = !shape.split && KindOf(program, var.type) != TypeKind::Array &&
                     shape.size <= kMaxZpGlobal;
        if (var.function < 0 && var.kind == VarKind::Global && !var.placement.has_address &&
            !var.placement.in_aux && small) {
            AddVarItem(be, items, static_cast<std::int32_t>(v), -1);
        }
    }
    for (std::size_t f = 0; f < fn_count; f++) {
        const Function& fn = program.functions[f];
        std::vector<std::int32_t> vars = fn.params;
        vars.insert(vars.end(), fn.locals.begin(), fn.locals.end());
        for (std::int32_t v : vars) {
            std::size_t at = static_cast<std::size_t>(v);
            bool made = program.vars[at].made;
            if (shares.slot_of[at] >= 0 || (made && shares.unnamed[at])) {
                continue;
            }
            AddVarItem(be, items, v, static_cast<std::int32_t>(f));
            if (shares.unnamed[at] && !items.empty() && items.back().var == v) {
                items.back().phantom = true;
                items.back().forced = false;
                items.back().weight = 0;
            }
        }
        Item temps;
        temps.sym = be.temp_sym[f];
        temps.function = static_cast<std::int32_t>(f);
        temps.size = be.temp_size[f];
        temps.weight = be.info[static_cast<std::size_t>(temps.sym)].weight;
        Item ptemps = temps;
        ptemps.sym = be.ptemp_sym[f];
        ptemps.size = be.ptemp_size[f];
        ptemps.forced = true;
        if (temps.size > 0) {
            items.push_back(temps);
        }
        if (ptemps.size > 0) {
            items.push_back(ptemps);
        }
        Item lptrs = ptemps;
        lptrs.sym = be.lptr_sym[f];
        lptrs.size = be.lptr_size[f];
        if (lptrs.size > 0) {
            items.push_back(lptrs);
        }
    }

    for (std::size_t k = 0; k < shares.slots.size(); k++) {
        const FrameSlot& slot = shares.slots[k];
        Item item;
        item.var = slot.vars[0];
        item.sym = be.var_sym[static_cast<std::size_t>(item.var)];
        item.function = slot.home;
        item.size = slot.size;
        item.forced = slot.forced;
        item.weight = slot.weight;
        item.slot = static_cast<std::int32_t>(k);
        items.push_back(item);
    }

    std::vector<std::uint32_t> zp_sizes(fn_count, 0);
    std::uint32_t globals = 0;
    for (Item& item : items) {
        if (!item.forced) {
            continue;
        }
        item.zp = true;
        if (item.function < 0) {
            globals += item.size;
        } else {
            zp_sizes[static_cast<std::size_t>(item.function)] += item.size;
        }
    }
    if (StackSize(st, zp_sizes, nullptr) + globals > length) {
        AddError(*be.errors, Loc(),
                 "the dereferenced pointers do not all fit in zero page; use fewer pointer "
                 "variables at once");
        return false;
    }
    // the rest by uses per byte, while they fit
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < items.size(); i++) {
        if (!items[i].forced && items[i].weight > 0) {
            order.push_back(i);
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) {
        return items[x].weight * items[y].size > items[y].weight * items[x].size;
    });
    for (std::size_t i : order) {
        Item& item = items[i];
        if (item.function < 0) {
            if (StackSize(st, zp_sizes, nullptr) + globals + item.size <= length) {
                item.zp = true;
                globals += item.size;
            }
            continue;
        }
        std::size_t f = static_cast<std::size_t>(item.function);
        zp_sizes[f] += item.size;
        if (StackSize(st, zp_sizes, nullptr) + globals <= length) {
            item.zp = true;
        } else {
            zp_sizes[f] -= item.size;
        }
    }

    // addresses: the zero-page stack, then the zero-page globals; memory frames by offset
    std::vector<std::uint32_t> mem_sizes(fn_count, 0);
    std::vector<std::uint32_t> zp_used(fn_count, 0);
    PlaceInFrames(program, items, zp_used, mem_sizes);
    std::vector<std::uint32_t> zp_off;
    std::vector<std::uint32_t> mem_off;
    plan.zp_frames = StackSize(st, zp_used, &zp_off);
    plan.mem_frames = StackSize(st, mem_sizes, &mem_off);
    plan.zp_globals_base = run + plan.zp_frames;
    std::uint32_t next_global = plan.zp_globals_base;
    for (Item& item : items) {
        AsmSymbol& sym = be.as.symbols[static_cast<std::size_t>(item.sym)];
        if (item.function < 0) {
            if (!item.zp) {
                continue;
            }
            sym.zp = true;
            DefineSymbol(be.as, item.sym, next_global);
            plan.zp_globals.push_back(item.var);
            next_global += item.size;
            continue;
        }
        std::size_t f = static_cast<std::size_t>(item.function);
        // the symbols of these bytes: the item's, or those of every var in its slot
        std::vector<std::int32_t> syms = {item.sym};
        if (item.slot >= 0) {
            syms.clear();
            for (std::int32_t v : shares.slots[static_cast<std::size_t>(item.slot)].vars) {
                syms.push_back(be.var_sym[static_cast<std::size_t>(v)]);
            }
        }
        for (std::int32_t s : syms) {
            be.as.symbols[static_cast<std::size_t>(s)].zp = item.zp;
            if (item.zp) {
                DefineSymbol(be.as, s, run + zp_off[f] + item.offset);
            } else {
                plan.mem_items.push_back({s, mem_off[f] + item.offset});
            }
        }
    }
    // a made local no code names: any address (the layout wants one)
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        if (shares.unnamed[v] && program.vars[v].made) {
            plan.mem_items.push_back({be.var_sym[v], 0});
        }
    }
    plan.zp_globals_size = next_global - plan.zp_globals_base;
    plan.zp_used = kScratchBytes + plan.zp_frames + plan.zp_globals_size;
    return true;
}

} // namespace tisple
