// what the lib/mockingboard tests share (test_mockingboard.hpp)

#include "machine/mockingboard.hpp"
#include "test_mockingboard.hpp"

#include <cmath>
#include <cstdio>

namespace tisple_tests {

void MbBuildText(TestResults& results, const std::string& path, const std::string& text,
                 MbBuild& b) {
    tisple::LoadOptions load;
    if (!text.empty()) {
        load.files.push_back(tisple::VirtualFile{path, text});
    }
    std::vector<tisple::Diagnostic> errors;
    b.ok = tisple::BuildProgram(path, load, b.program, errors) &&
           tisple::CompileProgram(b.program, tisple::BuildOptions(), b.code, errors);
    CHECK(results, b.ok);
    if (!b.ok) {
        std::fprintf(stderr, "%s", tisple::FormatDiagnostics(b.program.sources, errors).c_str());
    }
}

uint16_t MbVarAddress(const MbBuild& b, const char* name) {
    int32_t v = tisple::FindVar(b.program, name);
    return v < 0 ? 0 : b.code.layout.vars[static_cast<size_t>(v)].address;
}

std::unique_ptr<tisple::Apple2> MbStart(const MbBuild& b, int slot) {
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    std::string error;
    for (const tisple::MemorySegment& s : b.code.segments) {
        tisple::LoadSegment(*a, s, error);
    }
    tisple::LoadProgram(*a, b.code.image.data(), b.code.image.size(), b.code.origin, error);
    if (slot != 0) {
        tisple::InsertMockingboard(*a, slot);
    }
    a->lc.read_ram = true;
    a->lc.e000[0x1FFE] = static_cast<uint8_t>(b.code.irq_direct & 0xFF);
    a->lc.e000[0x1FFF] = static_cast<uint8_t>(b.code.irq_direct >> 8);
    return a;
}

void MeasureTick(void* ctx, const tisple::Machine& m) {
    TickCost& t = *static_cast<TickCost*>(ctx);
    uint16_t pc = tisple::GetRegs(m).pc;
    if (pc == t.entry && !t.inside) {
        t.inside = true;
        t.began = m.cycles - 7;
        return;
    }
    if (t.inside && m.mem[pc] == 0x40) {
        uint64_t took = m.cycles + 6 - t.began;
        t.inside = false;
        t.count++;
        t.total += took;
        t.most = took > t.most ? took : t.most;
        t.each.push_back(took);
    }
}

namespace {

const int kVoiceTone[6] = {0, 2, 4, 0, 2, 4};

} // namespace

Voices ReadVoices(const tisple::Apple2& a, uint64_t first_tick, int ticks) {
    Voices out;
    uint8_t regs[2][16] = {};
    const tisple::MockingboardState& mb = a.slots.mockingboard;
    uint64_t n = 0;
    for (int t = 0; t < ticks; t++) {
        uint64_t until = first_tick + static_cast<uint64_t>(t) * kMbTick + kMbTick / 2;
        while (n < mb.log_count && tisple::MockingboardWrite(a, n).cycle < until) {
            const tisple::PsgWrite& w = tisple::MockingboardWrite(a, n);
            if (w.reg < 16) {
                regs[w.chip][w.reg] = w.value;
            }
            n++;
        }
        for (int v = 0; v < 6; v++) {
            const uint8_t* r = regs[v / 3];
            out.period[v].push_back(
                static_cast<uint16_t>(r[kVoiceTone[v]] | (r[kVoiceTone[v] + 1] << 8)));
            out.volume[v].push_back(r[8 + v % 3]);
        }
        out.mixer[0].push_back(regs[0][7]);
        out.mixer[1].push_back(regs[1][7]);
    }
    return out;
}

uint16_t Period(int note) {
    double f = 440.0 * std::pow(2.0, (note - 45) / 12.0);
    return static_cast<uint16_t>(std::lround(1020484.0 / (16.0 * f)));
}

int Note(char letter, bool sharp, int octave) {
    const int semis[7] = {9, 11, 0, 2, 4, 5, 7}; // a b c d e f g
    return (octave - 1) * 12 + semis[letter - 'a'] + (sharp ? 1 : 0);
}

uint64_t FirstTick(const tisple::Apple2& a) {
    const tisple::MockingboardState& mb = a.slots.mockingboard;
    for (uint64_t n = 0; n < mb.log_count; n++) {
        const tisple::PsgWrite& w = tisple::MockingboardWrite(a, n);
        if (w.chip == 0 && w.reg == 2 && w.value != 0) {
            return w.cycle - 2000; // inside the first tick, before its writes
        }
    }
    return 0;
}

} // namespace tisple_tests
