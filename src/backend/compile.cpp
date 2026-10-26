#include "backend/frames.hpp"
#include "backend/peephole.hpp"
#include "backend/runtime.hpp"
#include "link/image.hpp"

#include <algorithm>
#include <cstdio>

namespace tisple {

namespace {

// a variable's bytes, as the dead-store pass may see them from this function's code: its own
// locals and parameters and every function's locals are read only by code that writes them
// first (a callee's locals by its inlined copies, or by itself); globals and other
// functions' parameters by any code after a call or the return
StoreScope VarScope(const Var& var, std::int32_t function) {
    if (var.address_taken || var.placement.has_address) {
        return StoreScope::Untracked;
    }
    if (var.kind == VarKind::Local || (var.kind == VarKind::Param && var.function == function)) {
        return StoreScope::Private;
    }
    if (var.kind == VarKind::Global || var.kind == VarKind::Param) {
        return StoreScope::Exposed;
    }
    return StoreScope::Untracked;
}

// by symbol, for this function's code
std::vector<StoreScope> StoreScopes(const Backend& be, std::int32_t function) {
    std::vector<StoreScope> out(be.info.size(), StoreScope::Untracked);
    for (std::size_t s = 0; s < be.info.size(); s++) {
        const SymInfo& info = be.info[s];
        if (info.role == SymRole::Var) {
            out[s] = VarScope(be.program->vars[static_cast<std::size_t>(info.owner)], function);
            continue;
        }
        bool frame = info.role == SymRole::Temps || info.role == SymRole::PtrTemps ||
                     info.role == SymRole::LoopPtrs;
        if (frame && info.owner == function) {
            out[s] = StoreScope::Private;
        }
    }
    return out;
}

// explicit-address initial values that are not in the file are stored by the startup
constexpr std::uint32_t kMaxStartupStores = 256;

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "$%04X", value);
    return text;
}

void MakeSymbols(Backend& be) {
    const Program& program = *be.program;
    be.var_sym.resize(program.vars.size());
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const Var& var = program.vars[v];
        std::string name = var.name;
        if (var.function >= 0) {
            name = program.functions[static_cast<std::size_t>(var.function)].name + "." + name;
        }
        std::int32_t sym = NewSym(be, name, SymRole::Var, static_cast<std::int32_t>(v));
        be.var_sym[v] = sym;
        AsmSymbol& s = be.as.symbols[static_cast<std::size_t>(sym)];
        if (var.placement.has_address) {
            DefineSymbol(be.as, sym, var.placement.address);
            s.zp = var.placement.address + be.shapes.vars[v].size <= 0x100;
        } else if (PointerInZp(be, static_cast<std::int32_t>(v))) {
            s.zp = true;
        }
    }
    std::size_t fn_count = program.functions.size();
    be.fn_sym.resize(fn_count);
    be.temp_sym.resize(fn_count);
    be.ptemp_sym.resize(fn_count);
    be.temp_size.assign(fn_count, 0);
    be.ptemp_size.assign(fn_count, 0);
    be.lptr_sym.resize(fn_count);
    be.lptr_size.assign(fn_count, 0);
    be.code.resize(fn_count);
    for (std::size_t f = 0; f < fn_count; f++) {
        const std::string& name = program.functions[f].name;
        std::int32_t owner = static_cast<std::int32_t>(f);
        be.fn_sym[f] = NewSym(be, name, SymRole::Label, owner);
        be.temp_sym[f] = NewSym(be, name + ".temps", SymRole::Temps, owner);
        be.ptemp_sym[f] = NewSym(be, name + ".ptrs", SymRole::PtrTemps, owner);
        be.as.symbols[static_cast<std::size_t>(be.ptemp_sym[f])].zp = true;
        be.lptr_sym[f] = NewSym(be, name + ".loop_ptrs", SymRole::LoopPtrs, owner);
        be.as.symbols[static_cast<std::size_t>(be.lptr_sym[f])].zp = true;
    }
    be.scratch = NewSym(be, "scratch", SymRole::Scratch, -1);
    be.rt_a = NewSym(be, "rt_a", SymRole::Scratch, -1);
    be.rt_b = NewSym(be, "rt_b", SymRole::Scratch, -1);
    be.rt_r = NewSym(be, "rt_r", SymRole::Scratch, -1);
    be.rt_s = NewSym(be, "rt_s", SymRole::Scratch, -1);
    for (std::int32_t s : {be.scratch, be.rt_a, be.rt_b, be.rt_r, be.rt_s}) {
        be.as.symbols[static_cast<std::size_t>(s)].zp = true;
    }
    const char* names[kRuntimeCount] = {"rt.mul8",   "rt.mul16", "rt.divu8",
                                        "rt.divu16", "rt.divs8", "rt.divs16"};
    for (int r = 0; r < kRuntimeCount; r++) {
        be.rt_sym[r] = NewSym(be, names[r], SymRole::Label, -1);
    }
}

// explicit-address globals with initial values outside [start, end): the startup writes them
bool StartupStores(const Backend& be, std::uint32_t start, std::uint32_t end,
                   std::vector<std::pair<std::uint16_t, std::uint8_t>>& stores,
                   std::vector<Diagnostic>& errors) {
    const Program& program = *be.program;
    std::vector<std::uint8_t> memory(0x10000, 0);
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const Var& var = program.vars[v];
        if (!var.placement.has_address || var.init.empty()) {
            continue;
        }
        std::uint32_t a = var.placement.address;
        std::uint32_t size = be.shapes.vars[v].size;
        if (a >= start && a + size <= end) {
            continue;
        }
        Layout placed = be.shapes;
        placed.vars[v].address = static_cast<std::uint16_t>(a);
        WriteVarInit(program, placed, static_cast<std::int32_t>(v), memory);
        for (std::uint32_t i = 0; i < size; i++) {
            std::uint16_t at = static_cast<std::uint16_t>((a + i) & 0xFFFF);
            stores.push_back({at, memory[at]});
        }
    }
    if (stores.size() > kMaxStartupStores) {
        AddError(errors, Loc(),
                 "more than 256 bytes of initial values live outside the binary (explicit "
                 ":address); give those globals no value and fill them in main");
        return false;
    }
    return true;
}

bool CheckCodeRange(std::uint32_t origin, std::uint32_t end, std::uint32_t limit,
                    std::vector<Diagnostic>& errors) {
    if (end > limit) {
        AddError(errors, Loc(),
                 "the code runs from " + Hex(origin) + " to " + Hex(end - 1) +
                     " and does not fit below " + Hex(limit) + "; use a lower --origin");
        return false;
    }
    for (const AddressRange& range : kReservedRanges) {
        if (range.end <= 0x200) {
            continue;
        }
        if (origin < range.end && range.start < end) {
            AddError(errors, Loc(),
                     "the code (" + Hex(origin) + "-" + Hex(end - 1) + ") overlaps " +
                         Hex(range.start) + "-" + Hex(range.end - 1) +
                         ", which the screen or I/O uses; pick another --origin");
            return false;
        }
    }
    return true;
}

// what one function's code added to the symbols' weights
using WeightsAdded = std::vector<std::pair<std::int32_t, std::uint64_t>>;

std::vector<std::uint64_t> Weights(const Backend& be) {
    std::vector<std::uint64_t> out(be.info.size());
    for (std::size_t s = 0; s < be.info.size(); s++) {
        out[s] = be.info[s].weight;
    }
    return out;
}

WeightsAdded AddedSince(const Backend& be, const std::vector<std::uint64_t>& before) {
    WeightsAdded out;
    for (std::size_t s = 0; s < be.info.size(); s++) {
        std::uint64_t old = s < before.size() ? before[s] : 0;
        if (be.info[s].weight != old) {
            out.push_back({static_cast<std::int32_t>(s), be.info[s].weight - old});
        }
    }
    return out;
}

// the function whose entry label this symbol is, or -1
std::int32_t FunctionOfSym(const Backend& be, std::int32_t sym) {
    if (sym < 0) {
        return -1;
    }
    const SymInfo& info = be.info[static_cast<std::size_t>(sym)];
    if (info.role != SymRole::Label || info.owner < 0) {
        return -1;
    }
    bool entry = be.fn_sym[static_cast<std::size_t>(info.owner)] == sym;
    return entry ? info.owner : -1;
}

// by function: its code is in the binary. Main and every function its code (after
// inlining) names, and theirs; with no main, or when asked, all of them
std::vector<bool> KeptFunctions(const Backend& be, const BuildOptions& options) {
    std::size_t count = be.program->functions.size();
    std::int32_t main = be.program->main;
    if (options.keep_all_functions || main < 0) {
        return std::vector<bool>(count, true);
    }
    std::vector<bool> kept(count, false);
    std::vector<std::int32_t> work = {main};
    kept[static_cast<std::size_t>(main)] = true;
    while (!work.empty()) {
        std::int32_t f = work.back();
        work.pop_back();
        for (const AsmItem& item : be.code[static_cast<std::size_t>(f)]) {
            std::int32_t callee = FunctionOfSym(be, item.value.sym);
            if (callee < 0 || kept[static_cast<std::size_t>(callee)]) {
                continue;
            }
            kept[static_cast<std::size_t>(callee)] = true;
            work.push_back(callee);
        }
    }
    return kept;
}

// a function no kept code calls: no code, no temporaries, its uses not counted when frames
// pick zero page. Its params and locals stay, since inlined copies of it use them
void DropFunction(Backend& be, std::int32_t function, const WeightsAdded& added) {
    std::size_t f = static_cast<std::size_t>(function);
    be.code[f].clear();
    be.temp_size[f] = 0;
    be.ptemp_size[f] = 0;
    be.lptr_size[f] = 0;
    for (const auto& [sym, weight] : added) {
        be.info[static_cast<std::size_t>(sym)].weight -= weight;
    }
}

// the runtime and division routines the kept code calls
void FindUsedRoutines(Backend& be) {
    std::vector<bool> named(be.info.size(), false);
    for (const std::vector<AsmItem>& code : be.code) {
        for (const AsmItem& item : code) {
            if (item.value.sym >= 0) {
                named[static_cast<std::size_t>(item.value.sym)] = true;
            }
        }
    }
    for (int r = 0; r < kRuntimeCount; r++) {
        be.rt_used[r] = named[static_cast<std::size_t>(be.rt_sym[r])];
    }
    std::vector<DivRoutine> used;
    for (const DivRoutine& r : be.div_routines) {
        if (named[static_cast<std::size_t>(r.sym)]) {
            used.push_back(r);
        }
    }
    be.div_routines = used;
}

void Append(std::vector<AsmItem>& to, const std::vector<AsmItem>& from, Compiled& out,
            std::int32_t function, const std::string& name) {
    FunctionSpan span;
    span.function = function;
    span.name = name;
    span.first = to.size();
    to.insert(to.end(), from.begin(), from.end());
    span.end = to.size();
    out.spans.push_back(span);
}

} // namespace

bool CompileProgram(const Program& source, const BuildOptions& options, Compiled& out,
                    std::vector<Diagnostic>& errors) {
    // the same program with its constant arithmetic folded: what the code is made from
    Program program = source;
    FoldConstants(program);
    out = Compiled();
    out.origin = options.origin;
    out.main = program.main;
    std::size_t before = errors.size();
    Backend be;
    be.program = &program;
    be.errors = &errors;
    if (!ShapeProgram(program, be.shapes, errors)) {
        return false;
    }
    be.deref = FindDerefPointers(program);
    be.aliases = FindIndexAliases(program);
    be.effects = ComputeEffects(program);
    be.div_pairs = FindDivPairs(program);
    be.ranges = FindValueRanges(program);
    be.fn_exprs.assign(program.functions.size(), 0);
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        be.fn_exprs[f] = CountExprs(program, program.functions[f].body);
    }
    MakeSymbols(be);
    std::vector<WeightsAdded> added(program.functions.size());
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        std::vector<std::uint64_t> weights = Weights(be);
        GenFunction(be, static_cast<std::int32_t>(f));
        Peephole(be.as, be.code[f], StoreScopes(be, static_cast<std::int32_t>(f)));
        added[f] = AddedSince(be, weights);
    }
    if (errors.size() != before) {
        return false;
    }
    std::vector<bool> kept = KeptFunctions(be, options);
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (!kept[f]) {
            DropFunction(be, static_cast<std::int32_t>(f), added[f]);
        }
    }
    FindUsedRoutines(be);
    std::vector<AsmItem> runtime;
    EmitRuntime(be, runtime);
    EmitDivRoutines(be, runtime);
    FramePlan plan;
    if (!AllocateFrames(be, plan)) {
        return false;
    }
    std::uint32_t data_end = options.limit - 256;

    StartupPlan sp;
    sp.main_sym = program.main >= 0 ? be.fn_sym[static_cast<std::size_t>(program.main)] : -1;
    sp.save_sym = NewSym(be, "zp_save", SymRole::Data, -1);
    sp.image_sym = NewSym(be, "zp_image", SymRole::Data, -1);
    sp.ready_sym = NewSym(be, "start.ready", SymRole::Label, -1);
    sp.after_sym = NewSym(be, "start.after_main", SymRole::Label, -1);
    sp.zp_base = plan.zp_globals_base;
    sp.zp_count = plan.zp_globals_size;
    if (!StartupStores(be, options.origin, data_end, sp.stores, errors)) {
        return false;
    }
    std::vector<AsmItem> startup;
    EmitStartup(be, sp, startup);
    std::vector<AsmItem> zp_image;
    zp_image.push_back(MakeLabel(sp.image_sym));
    for (std::uint32_t i = 0; i < sp.zp_count; i++) {
        zp_image.push_back(MakeByte(Num(0)));
    }

    std::vector<AsmItem>& items = be.as.items;
    Append(items, startup, out, -1, "startup");
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (kept[f]) {
            Append(items, be.code[f], out, static_cast<std::int32_t>(f), program.functions[f].name);
        }
    }
    Append(items, runtime, out, -1, "runtime");
    Append(items, zp_image, out, -1, "zero-page image");
    std::uint32_t code_end = LayoutCode(be.as, options.origin);
    if (!CheckCodeRange(options.origin, code_end, data_end, errors)) {
        return false;
    }
    for (const auto& item : plan.mem_items) {
        DefineSymbol(be.as, item.first, code_end + item.second);
    }
    std::uint32_t frames_end = code_end + plan.mem_frames;

    LayoutOptions lo;
    lo.data_start = frames_end;
    lo.data_end = data_end;
    lo.preset.assign(program.vars.size(), -1);
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const Var& var = program.vars[v];
        const AsmSymbol& sym = be.as.symbols[static_cast<std::size_t>(be.var_sym[v])];
        bool frame = var.function >= 0;
        bool zp_global = var.function < 0 && sym.defined && !var.placement.has_address;
        if ((frame || zp_global) && sym.defined) {
            lo.preset[v] = static_cast<std::int32_t>(sym.value);
        }
    }
    lo.reserved.push_back(AddressRange{options.origin, frames_end});
    lo.reserved.push_back(AddressRange{data_end, options.limit});
    if (!LayoutProgram(program, lo, out.layout, errors)) {
        return false;
    }
    std::uint32_t image_end = frames_end;
    std::vector<std::uint8_t> memory(0x10000, 0);
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const VarPlace& place = out.layout.vars[v];
        std::int32_t sym = be.var_sym[v];
        if (!be.as.symbols[static_cast<std::size_t>(sym)].defined) {
            DefineSymbol(be.as, sym, place.address);
        }
        WriteVarInit(program, out.layout, static_cast<std::int32_t>(v), memory);
        bool in_file = place.address >= options.origin && place.address < data_end;
        // an :address global with no value is left as memory holds it, like a screen
        const Var& var = program.vars[v];
        bool untouched = var.placement.has_address && var.init.empty();
        if (var.function < 0 && in_file && place.size > 0 && !untouched) {
            image_end = std::max(image_end, place.address + place.size);
        }
    }
    // the zero-page image holds the zero-page globals' starting values
    std::size_t image_at = out.spans.back().first + 1;
    for (std::uint32_t i = 0; i < sp.zp_count; i++) {
        items[image_at + i].value = Num(memory[sp.zp_base + i]);
    }
    DefineSymbol(be.as, sp.save_sym, data_end);

    std::vector<std::uint8_t> code;
    std::string error;
    if (!EncodeCode(be.as, code, error)) {
        AddError(errors, Loc(), "internal: " + error);
        return false;
    }
    out.image.assign(memory.begin() + options.origin, memory.begin() + image_end);
    std::copy(code.begin(), code.end(), out.image.begin());
    for (std::uint32_t a = code_end; a < frames_end; a++) {
        out.image[a - options.origin] = 0;
    }

    out.function_address.resize(program.functions.size());
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (kept[f]) {
            out.function_address[f] = static_cast<std::uint16_t>(
                be.as.symbols[static_cast<std::size_t>(be.fn_sym[f])].value);
        }
    }
    out.entry_ready =
        static_cast<std::uint16_t>(be.as.symbols[static_cast<std::size_t>(sp.ready_sym)].value);
    out.after_main =
        static_cast<std::uint16_t>(be.as.symbols[static_cast<std::size_t>(sp.after_sym)].value);
    out.save_buffer = static_cast<std::uint16_t>(data_end);
    out.code_bytes = code_end - options.origin;
    out.data_bytes = image_end - code_end;
    out.zp_bytes = plan.zp_used;
    out.zp_frames = plan.zp_frames;
    out.mem_frames = plan.mem_frames;
    out.as = std::move(be.as);
    return true;
}

} // namespace tisple
