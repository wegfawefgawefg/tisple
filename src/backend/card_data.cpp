// Data in the card's bank 1 and the functions that switch the card: card_data.hpp.

#include "backend/card_data.hpp"

#include "backend/card_bank.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace tisple {

bool SwitchesCard(const AsmItem& item) {
    if (item.kind != ItemKind::Ins || item.value.sym >= 0) {
        return false;
    }
    // at a constant address: $C080 indexed is a slot's I/O ($C080 + slot * 16), not the card's
    std::uint32_t address = static_cast<std::uint32_t>(item.value.offset) & 0xFFFF;
    return item.mode == Mode::Abs && (address & 0xFFF0) == 0xC080;
}

namespace {

bool IsCall(const AsmItem& item) {
    return item.kind == ItemKind::Ins && (item.mn == Mn::JSR || item.mn == Mn::JMP) &&
           item.mode == Mode::Abs && item.value.sym >= 0;
}

} // namespace

CardSwitching FindCardSwitching(const Backend& be, const std::vector<bool>& kept,
                                const std::vector<AsmItem>& runtime) {
    std::size_t count = be.code.size();
    CardSwitching out;
    out.functions.assign(count, false);
    std::map<std::int32_t, std::size_t> entry; // a function's entry label: the function
    for (std::size_t f = 0; f < count && f < be.fn_sym.size(); f++) {
        if (f < kept.size() && kept[f] && be.fn_sym[f] >= 0) {
            entry[be.fn_sym[f]] = f;
        }
    }
    std::set<std::int32_t> runtime_labels;
    for (const AsmItem& item : runtime) {
        if (item.kind == ItemKind::Label) {
            runtime_labels.insert(item.value.sym);
        }
    }
    // the switchers, and what they call while bank 1 is in (in the code's order: the switch
    // forms are straight-line, an if around each switch at most)
    std::vector<std::size_t> work;
    auto mark = [&](std::size_t f) {
        if (!out.functions[f]) {
            out.functions[f] = true;
            work.push_back(f);
        }
    };
    std::vector<bool> whole(count, false); // everything it calls counts, not only while in
    for (std::size_t f = 0; f < count; f++) {
        if (f >= kept.size() || !kept[f]) {
            continue;
        }
        bool in = false;
        for (const AsmItem& item : be.code[f]) {
            if (SwitchesCard(item)) {
                out.functions[f] = true;
                in = (static_cast<std::uint32_t>(item.value.offset) & 0x08) != 0;
                continue;
            }
            if (!in || !IsCall(item)) {
                continue;
            }
            auto found = entry.find(item.value.sym);
            if (found != entry.end()) {
                whole[found->second] = true;
            } else if (runtime_labels.count(item.value.sym) != 0) {
                out.runtime = true;
            }
        }
    }
    for (std::size_t f = 0; f < count; f++) {
        if (whole[f]) {
            out.functions[f] = false;
            mark(f);
        }
    }
    while (!work.empty()) {
        std::size_t f = work.back();
        work.pop_back();
        if (!whole[f]) {
            continue; // a switcher alone: only what it calls while bank 1 is in
        }
        for (const AsmItem& item : be.code[f]) {
            if (!IsCall(item)) {
                continue;
            }
            auto found = entry.find(item.value.sym);
            if (found != entry.end()) {
                whole[found->second] = true;
                mark(found->second);
            } else if (runtime_labels.count(item.value.sym) != 0) {
                out.runtime = true;
            }
        }
    }
    if (std::getenv("TISPLE_SWITCH_NOTES") != nullptr) {
        for (std::size_t f = 0; f < count; f++) {
            if (out.functions[f]) {
                std::fprintf(stderr, "switched: %s%s\n", be.program->functions[f].name.c_str(),
                             whole[f] ? " (called while bank 1 is in)" : "");
            }
        }
    }
    return out;
}

std::vector<bool> CardSwitchers(const Backend& be, const std::vector<bool>& kept,
                                const std::vector<AsmItem>& runtime) {
    return FindCardSwitching(be, kept, runtime).functions;
}

std::uint32_t Bank1DataBytes(const Program& program, const Layout& shapes) {
    std::uint32_t bytes = 0;
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        if (program.vars[v].placement.in_bank1) {
            bytes += shapes.vars[v].size;
        }
    }
    return bytes;
}

std::vector<std::int32_t> Bank1DataAddresses(const Program& program, const Layout& shapes) {
    std::vector<std::int32_t> out(program.vars.size(), -1);
    std::uint32_t at = kBank1End - Bank1DataBytes(program, shapes);
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        if (program.vars[v].placement.in_bank1) {
            out[v] = static_cast<std::int32_t>(at);
            at += shapes.vars[v].size;
        }
    }
    return out;
}

} // namespace tisple
