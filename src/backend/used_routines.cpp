// which runtime routines a program's kept code calls (runtime.hpp)

#include "backend/runtime.hpp"
#include "backend/squeeze.hpp"

#include <algorithm>

namespace tisple {

namespace {

// the quarter-square routines (1 KB of tables, about half the cycles) for every multiply when
// the multiplies of the kept code together may run more than once in a run of the program
// (every copy counted: inlined, unrolled; the tables serve them all), the shift-and-add ones
// otherwise. Squeezed, the tables are bytes the first multiply pulls in: only when the
// multiplies save the budget's hot cycles for each of them (80 cycles a multiply at least)
void UpgradeMultiplies(Backend& be) {
    const Runtime pairs[3][2] = {{Runtime::Mul8, Runtime::Mul8Q},
                                 {Runtime::Mul16, Runtime::Mul16Q},
                                 {Runtime::Mul8x8, Runtime::Mul8x8Q}};
    std::uint64_t total = 0;
    for (const auto& sites : be.multiplies) {
        for (const auto& [id, n] : sites) {
            total = std::min<std::uint64_t>(total + n, std::uint64_t(1) << 32);
        }
    }
    if (total < 2) {
        return;
    }
    const SizeBudget& budget = be.budget;
    std::uint64_t bytes = kQuarterSquareBytes;
    if (budget.squeeze > 0 && (budget.hot_cycles == 0 || total * 80 < budget.hot_cycles * bytes)) {
        return;
    }
    for (std::vector<AsmItem>& code : be.code) {
        for (AsmItem& item : code) {
            for (const auto& pair : pairs) {
                if (item.kind == ItemKind::Ins &&
                    item.value.sym == be.rt_sym[static_cast<int>(pair[0])]) {
                    item.value.sym = be.rt_sym[static_cast<int>(pair[1])];
                }
            }
        }
    }
}

} // namespace

void FindUsedRoutines(Backend& be) {
    UpgradeMultiplies(be);
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
    if (named[static_cast<std::size_t>(be.divu8_ge)]) {
        be.rt_used[static_cast<int>(Runtime::DivU8)] = true;
    }
    if (named[static_cast<std::size_t>(be.divu16_ge)]) {
        be.rt_used[static_cast<int>(Runtime::DivU16)] = true;
    }
    for (const DivRoutine& r : be.div_routines) {
        if (r.needs >= 0 && named[static_cast<std::size_t>(r.sym)]) {
            named[static_cast<std::size_t>(r.needs)] = true;
        }
    }
    std::vector<DivRoutine> used;
    for (const DivRoutine& r : be.div_routines) {
        if (named[static_cast<std::size_t>(r.sym)]) {
            used.push_back(r);
        }
    }
    be.div_routines = used;
}

} // namespace tisple
