// exact delays (asm/wait.hpp) run on the simulator: every constant count from 2 to 700 and a
// spread up to 65535 at addresses whose loop branches cross a page and ones whose do not,
// and variable waits at every address in a page, each to the cycle

#include "asm/wait.hpp"
#include "emu/harness.hpp"
#include "test_backend.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>

namespace tisple_tests {

namespace {

constexpr std::uint16_t kCount = 0x0300; // a variable wait's n
constexpr std::uint8_t kScratch = 0x80;

struct WaitRun {
    std::unique_ptr<tisple::Machine> m;
    int failures = 0;
};

// runs bytes at address up to the BRK after them; cycles, or -1 if a register, the stack
// pointer, zero page (but the scratch byte) or the code changed
std::int64_t RunWait(WaitRun& run, std::uint16_t address, const std::vector<std::uint8_t>& code,
                     std::uint8_t flags) {
    tisple::Machine& m = *run.m;
    for (int i = 0; i < 256; i++) {
        m.mem[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }
    tisple::LoadBytes(m, address, code.data(), code.size());
    std::uint16_t end = static_cast<std::uint16_t>(address + code.size());
    m.mem[end] = 0x00; // BRK
    tisple::Regs regs;
    regs.pc = address;
    regs.a = 0x5A;
    regs.x = 0xC3;
    regs.y = 0x96;
    regs.s = 0xF0;
    regs.p = flags;
    tisple::SetRegs(m, regs);
    tisple::RunOptions options;
    options.max_cycles = 400000;
    tisple::RunResult result = tisple::Run(m, options);
    bool kept = result.reason == tisple::StopReason::kBrk && result.regs.pc == end &&
                result.regs.a == regs.a && result.regs.x == regs.x && result.regs.y == regs.y &&
                result.regs.s == regs.s && (result.regs.p & tisple::kFlagD) == 0 &&
                (result.regs.p & tisple::kFlagI) == (flags & tisple::kFlagI);
    for (int i = 0; i < 256 && kept; i++) {
        kept = i == kScratch || m.mem[i] == static_cast<std::uint8_t>(i * 7 + 3);
    }
    for (std::size_t i = 0; i < code.size() && kept; i++) {
        kept = m.mem[static_cast<std::uint16_t>(address + i)] == code[i];
    }
    return kept ? static_cast<std::int64_t>(result.cycles) : -1;
}

// the low bytes of the addresses tried: every one near the end of a page, where the loops'
// branches cross, and a few others
std::vector<std::uint16_t> Addresses() {
    std::vector<std::uint16_t> out = {0x6000, 0x6001, 0x6080, 0x60C7, 0x0803, 0x95E0};
    for (std::uint16_t low = 0xE8; low <= 0xFF; low++) {
        out.push_back(static_cast<std::uint16_t>(0x6100 + low));
    }
    return out;
}

void CheckConstant(TestResults& results, WaitRun& run, std::uint32_t n,
                   const std::vector<std::uint16_t>& addresses) {
    int size = tisple::WaitSize(n);
    for (std::uint16_t address : addresses) {
        std::vector<std::uint8_t> code;
        tisple::WaitBytes(n, size, address, code);
        std::uint8_t flags = static_cast<std::uint8_t>(tisple::kFlagU | (n & 1 ? 0 : 0xC3));
        std::int64_t cycles = RunWait(run, address, code, flags);
        if (cycles != n || static_cast<int>(code.size()) != size) {
            if (run.failures++ < 10) {
                std::fprintf(stderr,
                             "wait %u at $%04X: %lld cycles, %zu bytes (size %d); -1 is a "
                             "register or memory changed\n",
                             n, address, static_cast<long long>(cycles), code.size(), size);
            }
            CHECK(results, cycles == n && static_cast<int>(code.size()) == size);
            return;
        }
    }
    results.passed++;
}

void TestConstantWaits(TestResults& results) {
    WaitRun run;
    run.m = std::make_unique<tisple::Machine>();
    tisple::InitMachine(*run.m);
    std::vector<std::uint16_t> addresses = Addresses();
    for (std::uint32_t n = tisple::kMinWait; n <= 700; n++) {
        CheckConstant(results, run, n, addresses);
    }
    for (std::uint32_t n = 701; n < tisple::kMaxWait; n += 997) {
        CheckConstant(results, run, n, addresses);
    }
    for (std::uint32_t n : {1284u, 1285u, 1286u, 1290u, 1296u, 4095u, 4096u, 32767u, 32768u, 65533u,
                            65534u, 65535u}) {
        CheckConstant(results, run, n, addresses);
    }
    // the sizes the planner picks: fewest bytes, so they never shrink much as n grows
    CHECK(results, tisple::WaitSize(2) == 1 && tisple::WaitSize(3) == 2);
    CHECK(results, tisple::WaitSize(7) == 2 && tisple::WaitSize(14) == 4);
    CHECK(results, tisple::WaitSize(1000) <= 11 && tisple::WaitSize(65535) <= 23);
    int worst = 0;
    for (std::uint32_t n = tisple::kMinWait; n <= tisple::kMaxWait; n += 101) {
        worst = std::max(worst, tisple::WaitSize(n));
    }
    std::printf("wait_cycles: 2 to 65535 cycles in at most %d bytes\n", worst);
    CHECK(results, worst <= 23);
}

void TestVariableWaits(TestResults& results) {
    WaitRun run;
    run.m = std::make_unique<tisple::Machine>();
    tisple::InitMachine(*run.m);
    std::vector<std::uint32_t> counts;
    for (std::uint32_t n = 0; n < 20; n++) {
        counts.push_back(n);
    }
    for (std::uint32_t n : {31u, 255u, 256u, 257u, 1000u, 0xABCDu}) {
        counts.push_back(n);
    }
    int failures = 0;
    for (std::uint32_t low = 0; low < 256; low++) {
        std::uint16_t address = static_cast<std::uint16_t>(0x6000 + low);
        std::vector<std::uint8_t> code;
        tisple::VarWaitBytes(address, kCount, kScratch, code);
        CHECK(results, static_cast<int>(code.size()) == tisple::VarWaitSize());
        for (std::uint32_t n : counts) {
            // the big ones at fewer addresses, to keep the run short
            if (n > 1000 && low % 16 != 15) {
                continue;
            }
            run.m->mem[kCount] = static_cast<std::uint8_t>(n & 0xFF);
            run.m->mem[kCount + 1] = static_cast<std::uint8_t>(n >> 8);
            std::int64_t cycles = RunWait(run, address, code, tisple::kFlagU | tisple::kFlagC);
            bool ok = cycles == n + tisple::kVarWaitOverhead && run.m->mem[kCount] == (n & 0xFF) &&
                      run.m->mem[kCount + 1] == (n >> 8);
            if (!ok && failures++ < 10) {
                std::fprintf(stderr, "variable wait %u at $%04X: %lld cycles, want %u\n", n,
                             address, static_cast<long long>(cycles), n + tisple::kVarWaitOverhead);
            }
            CHECK(results, ok);
        }
    }
    // every count at one address whose branches would cross without the filler
    std::vector<std::uint8_t> code;
    tisple::VarWaitBytes(0x60F5, kCount, kScratch, code);
    for (std::uint32_t n = 0; n < 65536; n += 251) {
        run.m->mem[kCount] = static_cast<std::uint8_t>(n & 0xFF);
        run.m->mem[kCount + 1] = static_cast<std::uint8_t>(n >> 8);
        std::int64_t cycles = RunWait(run, 0x60F5, code, tisple::kFlagU);
        CHECK(results, cycles == n + tisple::kVarWaitOverhead);
    }
}

} // namespace

void RunWaitTests(TestResults& results) {
    TestConstantWaits(results);
    TestVariableWaits(results);
}

} // namespace tisple_tests
