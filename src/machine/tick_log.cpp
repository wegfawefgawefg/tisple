#include "machine/tick_log.hpp"

#include <cstdio>

namespace tisple {

void StartTickLog(TickLog& log, const Apple2& a) {
    log.apple = &a;
    log.seen_taken = a.cpu.irqs_taken;
    log.low_at = UINT64_MAX;
    log.last_taken = 0;
    log.ticks.clear();
}

// called before each instruction: the line's fall noted at the first instruction after it, and
// an interrupt taken by the step before written down with how long the line had been low
void TickLogStep(void* ctx, const Machine& m) {
    TickLog& log = *static_cast<TickLog*>(ctx);
    if (m.irqs_taken != log.seen_taken) {
        log.seen_taken = m.irqs_taken;
        if (log.low_at != UINT64_MAX) {
            TickEntry e;
            e.taken = m.cycles;
            e.late = m.cycles - log.low_at;
            e.held = log.held;
            e.where = log.where;
            log.ticks.push_back(e);
        }
        log.low_at = UINT64_MAX;
        log.last_taken = m.cycles;
    }
    // a fall after the last take (until the handler answers, the line is still the old fall's)
    if (log.low_at == UINT64_MAX && m.irq_level_at != UINT64_MAX && m.cycles >= m.irq_level_at &&
        m.irq_level_at > log.last_taken) {
        log.low_at = m.irq_level_at;
        log.held = M6502_GET_ADDR(m.pins);
        log.where = FetchedFrom(*log.apple, log.held);
    }
}

bool WriteTickLog(const TickLog& log, const char* path, std::string& error) {
    FILE* file = std::fopen(path, "w");
    if (file == nullptr) {
        error = std::string("cannot write '") + path + "'";
        return false;
    }
    for (const TickEntry& e : log.ticks) {
        std::fprintf(file, "%llu %llu %04X %s\n", static_cast<unsigned long long>(e.taken),
                     static_cast<unsigned long long>(e.late), e.held, FetchedFromName(e.where));
    }
    return std::fclose(file) == 0;
}

std::string DescribeTickLog(const TickLog& log) {
    uint64_t latest = 0;
    for (const TickEntry& e : log.ticks) {
        latest = e.late > latest ? e.late : latest;
    }
    char text[96];
    std::snprintf(text, sizeof(text), "ticks: %zu, the latest %llu cycles late", log.ticks.size(),
                  static_cast<unsigned long long>(latest));
    return text;
}

} // namespace tisple
