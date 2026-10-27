// The link stage: the startup, the code and its frames, then the layout of every variable and
// the bytes of each segment. A program that fits from its origin comes out as it always did,
// one file. A bigger one goes on into the memory below the origin, then the language card,
// in that order: whole functions move to the next free stretch that holds them, and data
// fills what is left (backend.hpp has the memory map).

#include "backend/link.hpp"

#include "backend/runtime.hpp"
#include "link/image.hpp"

#include <algorithm>
#include <cstdio>

namespace tisple {

namespace {

// explicit-address initial values that are not in the file are stored by the startup
constexpr std::uint32_t kMaxStartupStores = 256;
constexpr std::uint32_t kCardStart = 0xD000;
constexpr std::uint32_t kCardEnd = 0x10000;

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "$%04X", value);
    return text;
}

// explicit-address globals with initial values outside [start, end): the startup writes them.
// Aux ones are in the aux segment instead
bool StartupStores(const Backend& be, std::uint32_t start, std::uint32_t end,
                   std::vector<std::pair<std::uint16_t, std::uint8_t>>& stores,
                   std::vector<Diagnostic>& errors) {
    const Program& program = *be.program;
    std::vector<std::uint8_t> memory(0x10000, 0);
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const Var& var = program.vars[v];
        if (!var.placement.has_address || var.init.empty() || var.placement.in_aux) {
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

// the origin itself must be ordinary memory
bool CheckOrigin(std::uint32_t origin, std::vector<Diagnostic>& errors) {
    for (const AddressRange& range : kReservedRanges) {
        if (range.end <= 0x200) {
            continue;
        }
        if (origin >= range.start && origin < range.end) {
            AddError(errors, Loc(),
                     "the origin " + Hex(origin) + " is in " + Hex(range.start) + "-" +
                         Hex(range.end - 1) +
                         ", which the screen or I/O uses; pick another --origin");
            return false;
        }
    }
    return true;
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

// [start, end) less every range in cuts
std::vector<AddressRange> Subtract(AddressRange range, const std::vector<AddressRange>& cuts) {
    std::vector<AddressRange> out = {range};
    for (const AddressRange& cut : cuts) {
        std::vector<AddressRange> next;
        for (const AddressRange& piece : out) {
            if (cut.end <= piece.start || cut.start >= piece.end) {
                next.push_back(piece);
                continue;
            }
            if (cut.start > piece.start) {
                next.push_back(AddressRange{piece.start, cut.start});
            }
            if (cut.end < piece.end) {
                next.push_back(AddressRange{cut.end, piece.end});
            }
        }
        out = next;
    }
    return out;
}

// the free stretches code and data may fill, in the order they are filled: from the origin,
// then below it, then the language card. Text page 1, I/O and every main-memory :address
// or :reserve let are cut out
std::vector<AddressRange> FreeStretches(const Backend& be, const BuildOptions& options,
                                        std::uint32_t data_end, bool card) {
    const Program& program = *be.program;
    std::vector<AddressRange> cuts(std::begin(kReservedRanges), std::end(kReservedRanges));
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const Var& var = program.vars[v];
        std::uint32_t size = be.shapes.vars[v].size;
        if (var.placement.has_address && !var.placement.in_aux && size > 0) {
            cuts.push_back(AddressRange{var.placement.address, var.placement.address + size});
        }
    }
    std::vector<AddressRange> out = Subtract(AddressRange{options.origin, data_end}, cuts);
    if (options.low_start < options.origin) {
        std::vector<AddressRange> low =
            Subtract(AddressRange{options.low_start, options.origin}, cuts);
        out.insert(out.end(), low.begin(), low.end());
    }
    if (card) {
        std::vector<AddressRange> high = Subtract(AddressRange{kCardStart, kCardEnd}, cuts);
        out.insert(out.end(), high.begin(), high.end());
    }
    return out;
}

// where each span's code goes: the stretch and its first address
struct CodePlace {
    std::size_t stretch = 0;
    std::uint32_t address = 0;
};

std::uint32_t SpanSize(const std::vector<AsmItem>& items, const FunctionSpan& span) {
    std::uint32_t size = 0;
    for (std::size_t i = span.first; i < span.end; i++) {
        size += items[i].size;
    }
    return size;
}

// spans in order, each whole in the first stretch from the current one on that holds it; the
// last span is followed by the memory frames. False when they do not fit
bool PlaceSpans(const std::vector<AsmItem>& items, const std::vector<FunctionSpan>& spans,
                const std::vector<AddressRange>& stretches, std::uint32_t mem_frames,
                std::vector<CodePlace>& places, std::uint32_t& frames_at) {
    places.assign(spans.size(), CodePlace());
    std::size_t k = 0;
    std::uint32_t pc = stretches.empty() ? 0 : stretches[0].start;
    for (std::size_t s = 0; s < spans.size(); s++) {
        std::uint32_t need = SpanSize(items, spans[s]) + (s + 1 == spans.size() ? mem_frames : 0);
        while (k < stretches.size() && pc + need > stretches[k].end) {
            k++;
            pc = k < stretches.size() ? stretches[k].start : 0;
        }
        if (k == stretches.size() || (s == 0 && k != 0)) {
            return false; // the startup must be at the origin
        }
        places[s] = CodePlace{k, pc};
        pc += SpanSize(items, spans[s]);
    }
    frames_at = pc;
    return true;
}

// lay out each run of spans in one stretch at its address; branches must stay in their run
bool LayOutRuns(Assembly& as, const std::vector<FunctionSpan>& spans,
                const std::vector<CodePlace>& places, std::vector<AddressRange>& runs,
                std::vector<Diagnostic>& errors) {
    std::size_t s = 0;
    while (s < spans.size()) {
        std::size_t end = s + 1;
        while (end < spans.size() && places[end].stretch == places[s].stretch) {
            end++;
        }
        std::size_t first_item = spans[s].first;
        std::size_t end_item = spans[end - 1].end;
        std::vector<AsmItem> run(as.items.begin() + static_cast<std::ptrdiff_t>(first_item),
                                 as.items.begin() + static_cast<std::ptrdiff_t>(end_item));
        std::swap(as.items, run);
        std::uint32_t run_end = LayoutCode(as, static_cast<std::uint16_t>(places[s].address));
        std::swap(as.items, run);
        std::copy(run.begin(), run.end(),
                  as.items.begin() + static_cast<std::ptrdiff_t>(first_item));
        runs.push_back(AddressRange{places[s].address, run_end});
        s = end;
    }
    for (const AsmItem& item : as.items) {
        if (item.kind != ItemKind::Ins || item.mode != Mode::Rel || item.value.sym < 0) {
            continue;
        }
        std::uint32_t target =
            as.symbols[static_cast<std::size_t>(item.value.sym)].value + item.value.offset;
        bool same_run = false;
        for (const AddressRange& run : runs) {
            bool from = item.address >= run.start && item.address < run.end;
            same_run = same_run || (from && target >= run.start && target < run.end);
        }
        if (!same_run) {
            AddError(errors, Loc(),
                     "internal: a branch at " + Hex(item.address) + " leaves its function's code");
            return false;
        }
    }
    return true;
}

// a segment's extent grows to cover [start, end)
void Cover(std::uint32_t& low, std::uint32_t& high, std::uint32_t start, std::uint32_t end) {
    low = std::min(low, start);
    high = std::max(high, end);
}

} // namespace

bool CardAllowed(const Program& program, const Layout& shapes) {
    for (const Expr& e : program.exprs) {
        std::uint16_t address = 0;
        if (e.kind == ExprKind::Jsr && ConstValue(program, e.a, address) && address >= kCardStart) {
            return false; // calls the rom there
        }
    }
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const Var& var = program.vars[v];
        std::uint32_t end = var.placement.address + shapes.vars[v].size;
        if (var.placement.has_address && !var.placement.in_aux && end > kCardStart) {
            return false; // reads the rom there, or I/O
        }
    }
    return true;
}

LinkResult LinkProgram(Backend& be, const LinkInput& in, bool card, Compiled& out,
                       std::vector<Diagnostic>& errors) {
    const Program& program = *be.program;
    const BuildOptions& options = *in.options;
    const FramePlan& plan = *in.plan;
    be.errors = &errors;
    out = Compiled();
    out.origin = options.origin;
    out.main = program.main;
    out.card_ram = card;
    std::uint32_t data_end = options.limit - 256;
    if (!CheckOrigin(options.origin, errors)) {
        return LinkResult::kError;
    }

    StartupPlan sp;
    sp.main_sym = program.main >= 0 ? be.fn_sym[static_cast<std::size_t>(program.main)] : -1;
    sp.save_sym = NewSym(be, "zp_save", SymRole::Data, -1);
    sp.image_sym = NewSym(be, "zp_image", SymRole::Data, -1);
    sp.ready_sym = NewSym(be, "start.ready", SymRole::Label, -1);
    sp.after_sym = NewSym(be, "start.after_main", SymRole::Label, -1);
    sp.zp_base = plan.zp_globals_base;
    sp.zp_count = plan.zp_globals_size;
    sp.card_ram = card;
    if (!StartupStores(be, options.origin, data_end, sp.stores, errors)) {
        return LinkResult::kError;
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
        if ((*in.kept)[f]) {
            Append(items, be.code[f], out, static_cast<std::int32_t>(f), program.functions[f].name);
        }
    }
    Append(items, *in.runtime, out, -1, "runtime");
    std::vector<AsmItem> tables;
    EmitRuntimeTables(be, tables);
    if (!tables.empty()) {
        Append(items, tables, out, -1, "runtime tables");
    }
    Append(items, zp_image, out, -1, "zero-page image");

    // the code where it fits: one run from the origin when it can be
    std::uint32_t code_end = LayoutCode(be.as, options.origin);
    std::vector<AddressRange> stretches = FreeStretches(be, options, data_end, card);
    std::uint32_t startup_end = options.origin + SpanSize(items, out.spans[0]);
    if (stretches.empty() || stretches[0].start != options.origin ||
        stretches[0].end < startup_end) {
        AddError(errors, Loc(),
                 "an :address let overlaps the program's startup (" + Hex(options.origin) + "-" +
                     Hex(startup_end - 1) + "); move it or pick another --origin");
        return LinkResult::kError;
    }
    std::vector<CodePlace> places;
    std::uint32_t frames_at = 0;
    if (!PlaceSpans(items, out.spans, stretches, plan.mem_frames, places, frames_at)) {
        std::string where;
        for (const AddressRange& stretch : stretches) {
            where += (where.empty() ? "" : ", ") + Hex(stretch.start) + "-" + Hex(stretch.end - 1);
        }
        AddError(errors, Loc(),
                 "the code and frames take " +
                     std::to_string(code_end - options.origin + plan.mem_frames) +
                     " bytes and do not fit in " + where + "; use a lower --origin or less code");
        return LinkResult::kNoRoom;
    }
    std::vector<AddressRange> runs;
    bool spilled = places.back().stretch != 0;
    if (spilled) {
        if (!LayOutRuns(be.as, out.spans, places, runs, errors)) {
            return LinkResult::kError;
        }
    } else {
        runs.push_back(AddressRange{options.origin, code_end});
    }
    for (const auto& item : plan.mem_items) {
        DefineSymbol(be.as, item.first, frames_at + item.second);
    }
    std::uint32_t frames_end = frames_at + plan.mem_frames;

    LayoutOptions lo;
    lo.data_start = spilled ? options.origin : frames_end;
    lo.data_end = data_end;
    if (options.low_start < options.origin) {
        lo.more_data.push_back(AddressRange{options.low_start, options.origin});
    }
    if (card) {
        lo.more_data.push_back(AddressRange{kCardStart, kCardEnd});
    }
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
    lo.reserved = runs;
    lo.reserved.push_back(AddressRange{frames_at, frames_end});
    lo.reserved.push_back(AddressRange{data_end, options.limit});
    if (!LayoutProgram(program, lo, out.layout, errors)) {
        return out.layout.no_room ? LinkResult::kNoRoom : LinkResult::kError;
    }

    // every byte: initial values, then the code over them, then zeroed frames
    std::vector<std::uint8_t> memory(0x10000, 0);
    std::vector<std::uint8_t> aux(0x10000, 0);
    // the file runs from the origin over everything in [origin, data_end); each other stretch
    // is a segment of what is in it
    std::uint32_t file_end = options.origin;
    std::vector<std::uint32_t> low(stretches.size(), 0x10000);
    std::vector<std::uint32_t> high(stretches.size(), 0);
    auto cover = [&](std::uint32_t start, std::uint32_t end) {
        if (start >= options.origin && end <= data_end) {
            file_end = std::max(file_end, end);
            return;
        }
        for (std::size_t k = 0; k < stretches.size(); k++) {
            if (start >= stretches[k].start && end <= stretches[k].end) {
                Cover(low[k], high[k], start, end);
            }
        }
    };
    for (const AddressRange& run : runs) {
        cover(run.start, run.end);
    }
    if (plan.mem_frames > 0) {
        cover(frames_at, frames_end);
    }
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const VarPlace& place = out.layout.vars[v];
        std::int32_t sym = be.var_sym[v];
        if (!be.as.symbols[static_cast<std::size_t>(sym)].defined) {
            DefineSymbol(be.as, sym, place.address);
        }
        const Var& var = program.vars[v];
        WriteVarInit(program, out.layout, static_cast<std::int32_t>(v),
                     var.placement.in_aux ? aux : memory);
        // an :address global with no value is left as memory holds it, like a screen
        bool untouched = var.placement.has_address && var.init.empty();
        if (var.function < 0 && place.size > 0 && !untouched && !var.placement.in_aux) {
            cover(place.address, place.address + place.size);
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
        return LinkResult::kError;
    }
    std::size_t at = 0;
    for (const AsmItem& item : items) {
        for (std::uint32_t i = 0; i < item.size; i++) {
            memory[(item.address + i) & 0xFFFF] = code[at++];
        }
    }
    std::fill(memory.begin() + frames_at, memory.begin() + frames_end, 0);

    out.image.assign(memory.begin() + options.origin, memory.begin() + file_end);
    for (std::size_t k = 0; k < stretches.size(); k++) {
        if (low[k] < high[k]) {
            MemoryBank bank =
                stretches[k].start >= kCardStart ? MemoryBank::kCard : MemoryBank::kMain;
            out.segments.push_back(SegmentOf(memory, bank, low[k], high[k]));
        }
    }
    AuxSegments(program, out.layout, aux, out.segments);

    out.function_address.resize(program.functions.size());
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if ((*in.kept)[f]) {
            out.function_address[f] = static_cast<std::uint16_t>(
                be.as.symbols[static_cast<std::size_t>(be.fn_sym[f])].value);
        }
    }
    out.entry_ready =
        static_cast<std::uint16_t>(be.as.symbols[static_cast<std::size_t>(sp.ready_sym)].value);
    out.after_main =
        static_cast<std::uint16_t>(be.as.symbols[static_cast<std::size_t>(sp.after_sym)].value);
    out.save_buffer = static_cast<std::uint16_t>(data_end);
    std::uint32_t total = static_cast<std::uint32_t>(out.image.size());
    for (const MemorySegment& segment : out.segments) {
        total += static_cast<std::uint32_t>(segment.bytes.size());
    }
    out.code_bytes = static_cast<std::uint32_t>(code.size());
    out.data_bytes = total - out.code_bytes;
    out.zp_bytes = plan.zp_used;
    out.zp_frames = plan.zp_frames;
    out.mem_frames = plan.mem_frames;
    return LinkResult::kOk;
}

} // namespace tisple
