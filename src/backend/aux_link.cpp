#include "backend/aux_link.hpp"

#include "backend/link_code.hpp"

#include <algorithm>

namespace tisple {

namespace {

constexpr std::uint32_t kMirrorEnd = 0xC000; // RAMRD switches $0200-$BFFF only

void Merge(std::vector<AddressRange>& ranges) {
    std::sort(ranges.begin(), ranges.end(),
              [](const AddressRange& a, const AddressRange& b) { return a.start < b.start; });
    std::vector<AddressRange> out;
    for (const AddressRange& r : ranges) {
        if (!out.empty() && r.start <= out.back().end) {
            out.back().end = std::max(out.back().end, r.end);
            continue;
        }
        out.push_back(r);
    }
    ranges = out;
}

void AddBelowMirrorEnd(std::vector<AddressRange>& ranges, std::uint32_t start, std::uint32_t end) {
    end = std::min(end, kMirrorEnd);
    if (start < end) {
        ranges.push_back(AddressRange{start, end});
    }
}

} // namespace

std::vector<bool> MirroredFunctions(const Backend& be, const std::vector<bool>& kept,
                                    const Bank1Plan* bank1) {
    std::size_t count = be.program->functions.size();
    std::vector<bool> out(count, false);
    for (std::size_t f = 0; f < count; f++) {
        bool banked = bank1 != nullptr && bank1->functions[f];
        out[f] = kept[f] && !banked && IsMirroredFunction(be.aux, static_cast<std::int32_t>(f));
    }
    return out;
}

bool MirrorsRuntime(const Backend& be, const std::vector<bool>& mirrored) {
    std::vector<bool> routine(be.info.size(), false);
    for (int r = 0; r < kRuntimeCount; r++) {
        routine[static_cast<std::size_t>(be.rt_sym[r])] = true;
    }
    for (std::int32_t sym : {be.divu8_ge, be.divu16_ge, be.sq_tables}) {
        routine[static_cast<std::size_t>(sym)] = true;
    }
    for (const DivRoutine& d : be.div_routines) {
        routine[static_cast<std::size_t>(d.sym)] = true;
    }
    for (std::size_t f = 0; f < mirrored.size(); f++) {
        if (!mirrored[f]) {
            continue;
        }
        for (const AsmItem& item : be.code[f]) {
            std::int32_t sym = item.value.sym;
            if (sym >= 0 && static_cast<std::size_t>(sym) < routine.size() &&
                routine[static_cast<std::size_t>(sym)]) {
                return true;
            }
        }
    }
    return false;
}

std::vector<AddressRange> SpanRanges(const std::vector<AsmItem>& items,
                                     const std::vector<FunctionSpan>& spans, std::size_t first,
                                     std::size_t end) {
    std::vector<AddressRange> out;
    for (std::size_t s = first; s < end && s < spans.size(); s++) {
        const FunctionSpan& span = spans[s];
        if (span.first >= span.end) {
            continue;
        }
        std::uint32_t start = items[span.first].address;
        std::uint32_t stop = start + SpanSize(items, span);
        AddBelowMirrorEnd(out, start, stop);
    }
    Merge(out);
    return out;
}

std::vector<AddressRange> MirroredConsts(const Backend& be, const Layout& layout,
                                         std::vector<bool>& mirrored) {
    const Program& program = *be.program;
    mirrored.assign(program.vars.size(), false);
    std::vector<AddressRange> out;
    if (!be.aux.reads) {
        return out;
    }
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const VarPlace& place = layout.vars[v];
        if (!be.aux.consts[v] || place.dropped || place.interp_only || place.size == 0 ||
            place.address >= kMirrorEnd) {
            continue;
        }
        mirrored[v] = true;
        AddBelowMirrorEnd(out, place.address, place.address + place.size);
    }
    Merge(out);
    return out;
}

bool AuxWithoutGuard(const Backend& be, const std::vector<bool>& kept, const Program& source,
                     Diagnostic& warning) {
    const Program& program = *be.program;
    if (!be.aux.any || program.handler < 0 || source.main < 0) {
        return false;
    }
    std::int32_t guard = FindFunction(source, "auxmem.guard");
    std::vector<bool> reached(source.functions.size(), false);
    std::vector<std::int32_t> work = {source.main};
    reached[static_cast<std::size_t>(source.main)] = true;
    while (!work.empty()) {
        std::int32_t f = work.back();
        work.pop_back();
        for (std::int32_t callee : source.functions[static_cast<std::size_t>(f)].callees) {
            if (!reached[static_cast<std::size_t>(callee)]) {
                reached[static_cast<std::size_t>(callee)] = true;
                work.push_back(callee);
            }
        }
    }
    if (guard >= 0 && reached[static_cast<std::size_t>(guard)]) {
        return false;
    }
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (kept[f] && be.aux.modes.owns[f] != 0) {
            warning.loc = program.functions[f].loc;
            warning.message =
                program.functions[f].name +
                " has a with_aux and the program an interrupt handler, but nothing calls "
                "(auxmem.guard): an interrupt inside the with_aux would run the handler from aux "
                "memory; call (auxmem.guard) once in main, or put (sei) and (cli) around it";
            return true;
        }
    }
    return false;
}

void CopyToAux(const std::vector<AddressRange>& ranges, const std::vector<std::uint8_t>& memory,
               std::vector<std::uint8_t>& aux) {
    for (const AddressRange& r : ranges) {
        std::copy(memory.begin() + r.start, memory.begin() + r.end, aux.begin() + r.start);
    }
}

} // namespace tisple
