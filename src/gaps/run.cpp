#include "gaps/run.hpp"

#include "backend/sim_run.hpp"
#include "emu/harness.hpp"
#include "machine/apple2.hpp"

namespace tisple {

namespace {

// the return address a call from nowhere pushes; reaching it means the code returned
constexpr std::uint16_t kReturnSentinel = 0xFFF0;

std::uint64_t NextRandom(std::uint64_t& s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

bool RunOnApple2(const Compiled& compiled, const ProfileRunOptions& options, Profiler& p,
                 ProfileRunResult& out, std::string& error) {
    std::unique_ptr<Apple2> a = NewApple2();
    for (const MemorySegment& segment : compiled.segments) {
        if (!LoadSegment(*a, segment, error)) {
            return false;
        }
    }
    if (!LoadProgram(*a, compiled.image.data(), compiled.image.size(), compiled.origin, error)) {
        return false;
    }
    SetBusLog(a->cpu, &p.bus);
    RunOutcome outcome = RunApple2Traced(*a, options.max_cycles, ProfileTrace, &p);
    FinishProfile(p, a->cpu);
    SetBusLog(a->cpu, nullptr);
    out.cycles = outcome.cycles;
    out.stop = outcome.reason == MachineStop::kRunning ? "cycle limit" : DescribeStop(*a);
    return true;
}

bool RunMainBare(const Compiled& compiled, const ProfileRunOptions& options, Profiler& p,
                 ProfileRunResult& out, std::string& error) {
    SimRunner run;
    if (!StartCompiled(run, compiled, error)) {
        return false;
    }
    Machine& m = *run.machine;
    // the inputs, written where tisple race writes them: the startup is done
    for (const RacePatch& patch : options.patches) {
        LoadBytes(m, patch.address, patch.bytes.data(), patch.bytes.size());
    }
    SetBusLog(m, &p.bus);
    RunOptions ro;
    ro.stop_pc = compiled.after_main;
    ro.max_cycles = options.max_cycles;
    ro.trace = ProfileTrace;
    ro.trace_ctx = &p;
    RunResult result = Run(m, ro);
    FinishProfile(p, m);
    SetBusLog(m, nullptr);
    out.cycles = result.cycles;
    out.stop =
        result.reason == StopReason::kStopAddress ? "main returned" : StopReasonName(result.reason);
    return true;
}

void WriteArgs(Machine& m, const Program& program, const Compiled& compiled, const Function& fn,
               std::uint64_t& rng) {
    for (std::int32_t param : fn.params) {
        std::uint16_t at = compiled.layout.vars[static_cast<std::size_t>(param)].address;
        std::uint32_t size =
            TypeOf(program, program.vars[static_cast<std::size_t>(param)].type).size;
        std::uint64_t r = NextRandom(rng);
        // mostly small numbers: counts and coordinates, the way programs call things
        std::uint16_t value = static_cast<std::uint16_t>((r >> 8) % 3 == 0 ? r >> 16 : r % 40);
        m.mem[at] = static_cast<std::uint8_t>(value & 0xFF);
        if (size == 2) {
            m.mem[static_cast<std::uint16_t>(at + 1)] = static_cast<std::uint8_t>(value >> 8);
        }
    }
}

// one call, traced; false when it did not come back
bool TracedCall(Machine& m, const Compiled& compiled, std::int32_t function,
                std::uint64_t max_cycles, Profiler& p, std::uint64_t& cycles) {
    Regs regs = GetRegs(m);
    std::uint16_t back = kReturnSentinel - 1;
    m.mem[0x1FF] = static_cast<std::uint8_t>(back >> 8);
    m.mem[0x1FE] = static_cast<std::uint8_t>(back & 0xFF);
    regs.s = 0xFD;
    regs.pc = compiled.function_address[static_cast<std::size_t>(function)];
    SetRegs(m, regs);
    RunOptions ro;
    ro.stop_pc = kReturnSentinel;
    ro.max_cycles = max_cycles;
    ro.trace = ProfileTrace;
    ro.trace_ctx = &p;
    RunResult result = Run(m, ro);
    FinishProfile(p, m);
    cycles += result.cycles;
    return result.reason == StopReason::kStopAddress;
}

bool RunLibrary(const Program& program, const Compiled& compiled, const ProfileRunOptions& options,
                Profiler& p, ProfileRunResult& out, std::string& error) {
    std::vector<std::int32_t> functions;
    for (const FunctionSpan& span : compiled.spans) {
        if (span.function >= 0 && span.end > span.first &&
            compiled.function_address[static_cast<std::size_t>(span.function)] != 0) {
            functions.push_back(span.function);
        }
    }
    if (functions.empty()) {
        error = "no main and no functions to call";
        return false;
    }
    SimRunner run;
    if (!StartCompiled(run, compiled, error)) {
        return false;
    }
    SetBusLog(*run.machine, &p.bus);
    std::uint64_t rng = options.seed * 0x9E3779B97F4A7C15ull + 1;
    out.stop = "cycle limit";
    std::uint64_t failures = 0;
    for (std::size_t i = 0; out.cycles < options.max_cycles; i++) {
        if (failures > 1000) {
            out.stop = "calls keep failing";
            break;
        }
        std::int32_t f = functions[i % functions.size()];
        const Function& fn = program.functions[static_cast<std::size_t>(f)];
        WriteArgs(*run.machine, program, compiled, fn, rng);
        out.calls++;
        if (TracedCall(*run.machine, compiled, f, options.call_cycles, p, out.cycles)) {
            ConsumeResult(p);
            continue;
        }
        failures++;
        // stuck or crashed: start again from a fresh machine, forgetting what was in flight
        ResetProfileTracking(p);
        if (!StartCompiled(run, compiled, error)) {
            return false;
        }
        SetBusLog(*run.machine, &p.bus);
    }
    return true;
}

} // namespace

bool ProfileCompiled(const Program& program, const Compiled& compiled,
                     const ProfileRunOptions& options, Profiler& profiler, ProfileRunResult& out,
                     std::string& error) {
    if (compiled.main < 0) {
        return RunLibrary(program, compiled, options, profiler, out, error);
    }
    if (options.bare) {
        return RunMainBare(compiled, options, profiler, out, error);
    }
    return RunOnApple2(compiled, options, profiler, out, error);
}

} // namespace tisple
