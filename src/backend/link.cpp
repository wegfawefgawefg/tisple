// The link stage: the startup, the code and its frames, then the layout of every variable and
// the bytes of each segment. A program that fits from its origin comes out as it always did,
// one file. A bigger one goes on into the memory below the origin, then the language card,
// in that order: whole functions move to the next free stretch that holds them, and data
// fills what is left (backend.hpp has the memory map).

#include "backend/link.hpp"

#include "backend/aux_link.hpp"
#include "backend/link_code.hpp"
#include "backend/link_data.hpp"
#include "backend/runtime.hpp"
#include "link/image.hpp"

#include <algorithm>
#include <cstdio>

namespace tisple {

namespace {

std::string Hex(std::uint32_t value) {
    return HexAddress(value);
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

// a segment's extent grows to cover [start, end)
void Cover(std::uint32_t& low, std::uint32_t& high, std::uint32_t start, std::uint32_t end) {
    low = std::min(low, start);
    high = std::max(high, end);
}

// with code in the card's bank 1, the stubs, the interrupt entry (the spans after the
// startup, before `resident`) and the frames in memory stay out of $D000-$DFFF
bool Bank1Resident(const std::vector<CodePlace>& places, std::size_t resident,
                   std::uint32_t frames_at, std::uint32_t frames_end,
                   std::vector<Diagnostic>& errors) {
    bool clear = frames_end <= kBank1Start || frames_at >= kBank1End || frames_at == frames_end;
    for (std::size_t s = 1; s < resident && s < places.size(); s++) {
        clear = clear && (places[s].address < kBank1Start || places[s].address >= kBank1End);
    }
    if (!clear) {
        AddError(errors, Loc(),
                 "no room for the language card's bank stubs and the frames outside $D000-$DFFF");
    }
    return clear;
}

} // namespace

bool CardAllowed(const Program& program, const Layout& shapes) {
    // a constant address from $D000 on in a jsr, an io_read or io_write, or a pointer made
    // with as: the program calls or reads the rom there
    for (const Expr& e : program.exprs) {
        bool addresses = e.kind == ExprKind::Jsr || e.kind == ExprKind::IoRead ||
                         e.kind == ExprKind::IoWrite || e.kind == ExprKind::Reinterpret;
        std::uint16_t address = 0;
        if (addresses && ConstValue(program, e.a, address) && address >= kCardStart) {
            return false;
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
    if (in.irq != nullptr && !in.irq->empty()) {
        sp.irq_entry = in.irq_plan.entry_sym;
        sp.irq_direct = in.irq_plan.direct_sym;
        sp.irq_saved = in.irq_plan.saved_sym;
    }
    for (std::int32_t f : be.io_bases.patches) {
        if ((*in.kept)[static_cast<std::size_t>(f)]) {
            sp.io_patches.push_back(be.fn_sym[static_cast<std::size_t>(f)]);
        }
    }
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

    // code in the card's bank 1: its calls through stubs, which go right after the startup with
    // the interrupt entry (neither may be in $D000-$DFFF), and its own code last
    const Bank1Plan* bank1 =
        card && in.bank1 != nullptr && in.bank1->bytes > 0 ? in.bank1 : nullptr;
    auto in_bank1 = [&](std::size_t f) { return bank1 != nullptr && bank1->functions[f]; };
    std::vector<AsmItem> runtime = *in.runtime;
    std::vector<AsmItem> irq;
    if (sp.irq_entry >= 0) {
        irq = bank1 != nullptr ? *in.irq_banked : *in.irq;
    }
    std::vector<AsmItem> stubs;
    std::vector<std::int32_t> forward;
    if (bank1 != nullptr) {
        RouteBank1(be, *bank1, *in.kept, startup, runtime, irq, stubs, forward);
    }
    std::vector<AsmItem>& items = be.as.items;
    Append(items, startup, out, -1, "startup");
    if (bank1 != nullptr) {
        Append(items, stubs, out, -1, "bank stubs");
        if (!irq.empty()) {
            Append(items, irq, out, -1, "interrupt entry");
        }
    }
    std::size_t resident_spans = out.spans.size();
    // code with_aux may run with RAMRD on first, with the runtime when it calls it: what the
    // link copies to aux memory (aux_link.hpp)
    std::vector<AsmItem> tables;
    EmitRuntimeTables(be, tables);
    std::vector<bool> mirrored = MirroredFunctions(be, *in.kept, bank1);
    bool runtime_mirrored = MirrorsRuntime(be, mirrored);
    std::size_t mirror_first = out.spans.size();
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (mirrored[f]) {
            Append(items, be.code[f], out, static_cast<std::int32_t>(f), program.functions[f].name);
        }
    }
    if (runtime_mirrored) {
        Append(items, runtime, out, -1, "runtime");
        if (!tables.empty()) {
            Append(items, tables, out, -1, "runtime tables");
        }
    }
    std::size_t mirror_end = out.spans.size();
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if ((*in.kept)[f] && !in_bank1(f) && !mirrored[f]) {
            Append(items, be.code[f], out, static_cast<std::int32_t>(f), program.functions[f].name);
        }
    }
    if (!runtime_mirrored) {
        Append(items, runtime, out, -1, "runtime");
    }
    if (!irq.empty() && bank1 == nullptr) {
        Append(items, irq, out, -1, "interrupt entry");
    }
    if (!tables.empty() && !runtime_mirrored) {
        Append(items, tables, out, -1, "runtime tables");
    }
    Append(items, zp_image, out, -1, "zero-page image");
    std::size_t image_span = out.spans.size() - 1;
    std::size_t first_bank1 = bank1 != nullptr ? out.spans.size() : SIZE_MAX;
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if ((*in.kept)[f] && in_bank1(f)) {
            Append(items, be.code[f], out, static_cast<std::int32_t>(f), program.functions[f].name);
            out.spans.back().card_bank1 = true;
        }
    }

    // the code where it fits: one run from the origin when it can be. A branch to another
    // stretch's code is made far, and the code placed again with its bigger size
    std::vector<AddressRange> stretches = FreeStretches(be, options, data_end, card);
    if (bank1 != nullptr) {
        stretches.push_back(AddressRange{kBank1Start, kBank1End});
    }
    // the stretches main memory and the card's bank 2 have, which data may fill too
    std::size_t shared = stretches.size() - (bank1 != nullptr ? 1 : 0);
    std::vector<CodePlace> places;
    std::uint32_t frames_at = 0;
    std::uint32_t code_end = 0;
    for (bool again = true; again;) {
        code_end = LayoutCode(be.as, options.origin);
        std::uint32_t startup_end = options.origin + SpanSize(items, out.spans[0]);
        if (stretches.empty() || stretches[0].start != options.origin ||
            stretches[0].end < startup_end) {
            AddError(errors, Loc(),
                     "an :address let overlaps the program's startup (" + Hex(options.origin) +
                         "-" + Hex(startup_end - 1) + "); move it or pick another --origin");
            return LinkResult::kError;
        }
        if (!PlaceSpans(items, out.spans, stretches, plan.mem_frames, places, frames_at,
                        first_bank1)) {
            std::string where;
            for (const AddressRange& stretch : stretches) {
                where +=
                    (where.empty() ? "" : ", ") + Hex(stretch.start) + "-" + Hex(stretch.end - 1);
            }
            AddError(errors, Loc(),
                     "the code and frames take " +
                         std::to_string(code_end - options.origin + plan.mem_frames) +
                         " bytes and do not fit in " + where +
                         "; use a lower --origin or less code");
            return LinkResult::kNoRoom;
        }
        again = MarkFarBranches(be.as, out.spans, places);
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
    AddressRange bank1_run;
    if (bank1 != nullptr) {
        bank1_run = runs.back();
        runs.pop_back();
        if (!Bank1Resident(places, resident_spans, frames_at, frames_end, errors)) {
            return LinkResult::kNoRoom;
        }
    }

    LayoutOptions lo;
    lo.data_start = spilled ? options.origin : frames_end;
    lo.data_end = data_end;
    if (options.low_start < options.origin) {
        lo.more_data.push_back(AddressRange{options.low_start, options.origin});
    }
    if (card) {
        lo.more_data.push_back(AddressRange{kCardStart, CardEnd(program)});
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
    // consts only the interpreter reads go inside the code, so the binary has no bytes of them
    TableUse use = FindTableUse(be, in);
    std::vector<VarPlace> interp_places;
    bool interp_fits = PlaceInterpOnly(be, use.interp_only, runs, interp_places);
    lo.dropped = use.dropped;
    if (bank1 != nullptr) {
        lo.avoiders = bank1->avoiders;
        lo.avoid = AddressRange{kBank1Start, kBank1End};
    }
    for (std::size_t v = 0; v < program.vars.size() && interp_fits; v++) {
        lo.dropped[v] = lo.dropped[v] || use.interp_only[v];
    }
    lo.reserved = runs;
    lo.reserved.push_back(AddressRange{frames_at, frames_end});
    lo.reserved.push_back(AddressRange{data_end, options.limit});
    std::vector<AddressRange> mirror = SpanRanges(items, out.spans, mirror_first, mirror_end);
    lo.aux_reserved = mirror;
    if (!LayoutProgram(program, lo, out.layout, errors)) {
        return out.layout.no_room ? LinkResult::kNoRoom : LinkResult::kError;
    }
    // the const data that code reads by name with RAMRD on is copied to aux memory too: laid
    // out again with the lets :in aux kept off it (main memory's layout comes out the same)
    std::vector<bool> mirrored_consts;
    std::vector<AddressRange> consts = MirroredConsts(be, out.layout, mirrored_consts);
    if (!consts.empty()) {
        lo.aux_reserved.insert(lo.aux_reserved.end(), consts.begin(), consts.end());
        mirror.insert(mirror.end(), consts.begin(), consts.end());
        if (!LayoutProgram(program, lo, out.layout, errors)) {
            return out.layout.no_room ? LinkResult::kNoRoom : LinkResult::kError;
        }
    }
    out.layout.mirrored = mirrored_consts;
    for (std::size_t v = 0; v < program.vars.size() && interp_fits; v++) {
        if (use.interp_only[v]) {
            out.layout.vars[v] = interp_places[v];
        }
    }
    // a function's name as a value is its entry's address (dispatch.hpp), in the code and in
    // the const tables below
    out.layout.functions.assign(be.entries.size(), 0);
    for (std::size_t f = 0; f < be.entries.size(); f++) {
        std::size_t entry = static_cast<std::size_t>(be.entries[f]);
        if (be.entries[f] >= 0 && (*in.kept)[entry]) {
            out.layout.functions[f] = static_cast<std::uint16_t>(
                be.as.symbols[static_cast<std::size_t>(be.fn_sym[entry])].value);
        }
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
        for (std::size_t k = 0; k < shared; k++) {
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
        if (place.interp_only) {
            continue;
        }
        WriteVarInit(program, out.layout, static_cast<std::int32_t>(v),
                     var.placement.in_aux ? aux : memory);
        // an :address global with no value is left as memory holds it, like a screen
        bool untouched = var.placement.has_address && var.init.empty();
        if (var.function < 0 && place.size > 0 && !untouched && !var.placement.in_aux) {
            cover(place.address, place.address + place.size);
        }
    }
    // the zero-page image holds the zero-page globals' starting values
    std::size_t image_at = out.spans[image_span].first + 1;
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
    std::vector<std::uint8_t> bank1_memory(bank1 != nullptr ? 0x10000 : 0, 0);
    std::size_t bank1_items = bank1 != nullptr ? out.spans[first_bank1].first : items.size();
    std::size_t at = 0;
    for (std::size_t n = 0; n < items.size(); n++) {
        const AsmItem& item = items[n];
        std::vector<std::uint8_t>& to = n < bank1_items ? memory : bank1_memory;
        for (std::uint32_t i = 0; i < item.size; i++) {
            to[(item.address + i) & 0xFFFF] = code[at++];
        }
    }
    std::fill(memory.begin() + frames_at, memory.begin() + frames_end, 0);

    CopyToAux(mirror, memory, aux);
    out.image.assign(memory.begin() + options.origin, memory.begin() + file_end);
    for (std::size_t k = 0; k < shared; k++) {
        if (low[k] < high[k]) {
            MemoryBank bank =
                stretches[k].start >= kCardStart ? MemoryBank::kCard : MemoryBank::kMain;
            out.segments.push_back(SegmentOf(memory, bank, low[k], high[k]));
        }
    }
    if (bank1 != nullptr && bank1_run.start < bank1_run.end) {
        out.segments.push_back(
            SegmentOf(bank1_memory, MemoryBank::kCardBank1, bank1_run.start, bank1_run.end));
    }
    AuxSegments(program, out.layout, aux, mirror, out.segments);

    // a bank 1 function is called through its forward stub (none: only bank 1 calls it)
    out.function_address.resize(program.functions.size());
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        std::int32_t sym = in_bank1(f) ? forward[f] : be.fn_sym[f];
        if ((*in.kept)[f] && sym >= 0) {
            out.function_address[f] =
                static_cast<std::uint16_t>(be.as.symbols[static_cast<std::size_t>(sym)].value);
        }
    }
    // the card's switches in the code the link itself writes (not the program's own I/O)
    for (const FunctionSpan& span : out.spans) {
        for (std::size_t i = span.first; i < span.end && span.function < 0; i++) {
            const AsmItem& item = items[i];
            std::uint32_t address = static_cast<std::uint32_t>(item.value.offset);
            bool card_io = (address & 0xFFF0) == 0xC080 || address == kBankStatus;
            if (item.kind == ItemKind::Ins && item.value.sym < 0 && item.mode == Mode::Abs &&
                card_io) {
                out.card_switches.push_back(item.address);
            }
        }
    }
    out.entry_ready =
        static_cast<std::uint16_t>(be.as.symbols[static_cast<std::size_t>(sp.ready_sym)].value);
    out.after_main =
        static_cast<std::uint16_t>(be.as.symbols[static_cast<std::size_t>(sp.after_sym)].value);
    out.save_buffer = static_cast<std::uint16_t>(data_end);
    if (sp.irq_entry >= 0) {
        out.irq_entry =
            static_cast<std::uint16_t>(be.as.symbols[static_cast<std::size_t>(sp.irq_entry)].value);
        out.irq_direct = static_cast<std::uint16_t>(
            be.as.symbols[static_cast<std::size_t>(sp.irq_direct)].value);
    }
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
