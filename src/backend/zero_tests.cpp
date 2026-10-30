#include "asm/opcodes.hpp"
#include "backend/peephole.hpp"

#include <map>

// A loop that runs while a u16 is not zero tests it at the bottom with LDA lo / ORA hi /
// BNE top. Split, LDA lo / BNE top / LDA hi / BNE top goes back three cycles sooner whenever
// the low byte is not zero, and when the u16 was just stepped (a count down), DropDeadValues
// then drops the LDA lo, since the step's N and Z already show the byte. Falling out of the
// loop both leave A zero and the same flags; going back they differ in A and N, so only when
// nothing at the top reads them first. Two bytes more, so only for jumps back.

namespace tisple {

namespace {

std::size_t SkipNotes(const std::vector<AsmItem>& items, std::size_t i) {
    while (i < items.size() && items[i].kind == ItemKind::Note) {
        i++;
    }
    return i;
}

// mn of a RAM byte the program owns (not I/O: the split may skip its read)
bool IsPlainByte(const Assembly& as, const AsmItem& item, Mn mn) {
    if (item.kind != ItemKind::Ins || item.mn != mn ||
        (item.mode != Mode::Abs && item.mode != Mode::Zp) || item.value.part != Part::Whole ||
        item.value.sym < 0) {
        return false;
    }
    const AsmSymbol& sym = as.symbols[static_cast<std::size_t>(item.value.sym)];
    return !sym.defined || sym.value + 0x100 < 0xC000;
}

} // namespace

bool SplitZeroTests(const Assembly& as, std::vector<AsmItem>& items) {
    std::map<std::int32_t, std::size_t> label_at;
    for (std::size_t i = 0; i < items.size(); i++) {
        if (items[i].kind == ItemKind::Label) {
            label_at[items[i].value.sym] = i;
        }
    }
    std::vector<std::uint8_t> live;
    std::vector<std::size_t> splits; // the ORAs
    for (std::size_t i = 0; i < items.size(); i++) {
        if (!IsPlainByte(as, items[i], Mn::LDA)) {
            continue;
        }
        std::size_t ora = SkipNotes(items, i + 1);
        std::size_t bne = ora < items.size() ? SkipNotes(items, ora + 1) : items.size();
        if (bne >= items.size() || !IsPlainByte(as, items[ora], Mn::ORA) ||
            items[bne].kind != ItemKind::Ins || items[bne].mn != Mn::BNE) {
            continue;
        }
        auto top = label_at.find(items[bne].value.sym);
        if (top == label_at.end() || top->second > i) {
            continue;
        }
        if (live.empty()) {
            live = LiveRegsIn(as, items);
        }
        if ((live[top->second] & (kLiveA | kLiveN)) == 0) {
            splits.push_back(ora);
        }
    }
    if (splits.empty()) {
        return false;
    }
    std::vector<AsmItem> out;
    std::size_t next = 0;
    for (std::size_t i = 0; i < items.size(); i++) {
        if (next < splits.size() && splits[next] == i) {
            // LDA lo / ORA hi / BNE top: LDA lo / BNE top / LDA hi / BNE top
            std::size_t bne = SkipNotes(items, i + 1);
            AsmItem load = items[i];
            load.mn = Mn::LDA;
            out.push_back(items[bne]);
            out.push_back(load);
            next++;
            continue;
        }
        out.push_back(items[i]);
    }
    items.swap(out);
    return true;
}

} // namespace tisple
