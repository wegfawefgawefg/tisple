// Where the link puts the code: link_code.hpp.

#include "backend/link_code.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

namespace tisple {

std::string HexAddress(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "$%04X", value);
    return text;
}

namespace {

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

// the end of the run of spans in one stretch that starts at span s
std::size_t RunEnd(const std::vector<CodePlace>& places, std::size_t s) {
    std::size_t end = s + 1;
    while (end < places.size() && places[end].stretch == places[s].stretch) {
        end++;
    }
    return end;
}

} // namespace

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
        std::vector<AddressRange> high = Subtract(AddressRange{kCardStart, CardEnd(program)}, cuts);
        out.insert(out.end(), high.begin(), high.end());
    }
    return out;
}

std::uint32_t SpanSize(const std::vector<AsmItem>& items, const FunctionSpan& span) {
    std::uint32_t size = 0;
    for (std::size_t i = span.first; i < span.end; i++) {
        size += items[i].size;
    }
    return size;
}

bool PlaceSpans(const std::vector<AsmItem>& items, const std::vector<FunctionSpan>& spans,
                const std::vector<AddressRange>& stretches, std::uint32_t mem_frames,
                std::vector<CodePlace>& places, std::uint32_t& frames_at, std::size_t first_bank1) {
    places.assign(spans.size(), CodePlace());
    std::size_t last = first_bank1 < spans.size() ? first_bank1 : spans.size();
    std::size_t count = first_bank1 < spans.size() ? stretches.size() - 1 : stretches.size();
    std::size_t k = 0;
    std::uint32_t pc = stretches.empty() ? 0 : stretches[0].start;
    for (std::size_t s = 0; s < last; s++) {
        std::uint32_t need = SpanSize(items, spans[s]) + (s + 1 == last ? mem_frames : 0);
        while (k < count && pc + need > stretches[k].end) {
            k++;
            pc = k < count ? stretches[k].start : 0;
        }
        if (k == count || (s == 0 && k != 0)) {
            return false; // the startup must be at the origin
        }
        places[s] = CodePlace{k, pc};
        pc += SpanSize(items, spans[s]);
    }
    frames_at = pc;
    // the language card's bank 1: the last stretch, its own
    if (last < spans.size()) {
        k = stretches.size() - 1;
        pc = stretches[k].start;
        for (std::size_t s = last; s < spans.size(); s++) {
            places[s] = CodePlace{k, pc};
            pc += SpanSize(items, spans[s]);
        }
        if (pc > stretches[k].end) {
            return false;
        }
    }
    return true;
}

bool MarkFarBranches(Assembly& as, const std::vector<FunctionSpan>& spans,
                     const std::vector<CodePlace>& places) {
    bool changed = false;
    std::size_t s = 0;
    while (s < spans.size()) {
        std::size_t end = RunEnd(places, s);
        std::set<std::int32_t> labels;
        for (std::size_t i = spans[s].first; i < spans[end - 1].end; i++) {
            if (as.items[i].kind == ItemKind::Label) {
                labels.insert(as.items[i].value.sym);
            }
        }
        for (std::size_t i = spans[s].first; i < spans[end - 1].end; i++) {
            AsmItem& item = as.items[i];
            bool branch = item.kind == ItemKind::Ins && item.mode == Mode::Rel;
            if (branch && !item.far && item.value.sym >= 0 && !labels.count(item.value.sym)) {
                item.far = true;
                changed = true;
            }
        }
        s = end;
    }
    return changed;
}

bool LayOutRuns(Assembly& as, const std::vector<FunctionSpan>& spans,
                const std::vector<CodePlace>& places, std::vector<AddressRange>& runs,
                std::vector<Diagnostic>& errors) {
    std::size_t s = 0;
    while (s < spans.size()) {
        std::size_t end = RunEnd(places, s);
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
        // a long branch's JMP reaches anywhere
        bool branch = item.kind == ItemKind::Ins && item.mode == Mode::Rel;
        if (!branch || item.long_branch || item.value.sym < 0) {
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
                     "internal: a branch at " + HexAddress(item.address) +
                         " leaves its function's code");
            return false;
        }
    }
    return true;
}

} // namespace tisple
