#include "backend/sim_run.hpp"

#include <algorithm>
#include <utility>

namespace tisple {

namespace {

// the return address the harness pushes; reaching it means the code returned
constexpr std::uint16_t kSentinel = 0xFFF0;

// the language card's $D000 bank: the flat memory holds the one switched in, the runner the
// other. Its switches are the program's own business, not I/O to compare
void SwitchBank(SimRunner& run, bool bank2) {
    if (bank2 == run.bank2) {
        return;
    }
    std::uint8_t* d000 = run.machine->mem + 0xD000;
    for (std::size_t i = 0; i < run.other_bank.size(); i++) {
        std::swap(d000[i], run.other_bank[i]);
    }
    run.bank2 = bank2;
}

// an access by one of the build's card switches: an absolute operand, read with the
// instruction's three bytes fetched
bool BySwitch(const SimRunner& run) {
    std::uint16_t at = static_cast<std::uint16_t>(run.machine->cpu.PC - 3);
    return run.card && run.switch_at[at];
}

std::uint8_t IoRead(void* ctx, std::uint16_t address) {
    SimRunner& run = *static_cast<SimRunner*>(ctx);
    if ((address & 0xFFF0) == 0xC080 && BySwitch(run)) {
        SwitchBank(run, (address & 0x08) == 0);
        return 0;
    }
    if (address == 0xC011 && BySwitch(run)) {
        return run.bank2 ? 0x80 : 0x00; // RDLCBNK2
    }
    std::uint8_t value = IoReadValue(address, run.io.size());
    run.io.push_back(IoAccess{address, value, false});
    return value;
}

void IoWrite(void* ctx, std::uint16_t address, std::uint8_t value) {
    SimRunner& run = *static_cast<SimRunner*>(ctx);
    run.io.push_back(IoAccess{address, value, true});
}

// JSR from nowhere: the next RTS lands on the sentinel
void PushReturn(Machine& m, Regs& regs) {
    std::uint16_t back = kSentinel - 1;
    m.mem[0x100 + regs.s] = static_cast<std::uint8_t>(back >> 8);
    regs.s = static_cast<std::uint8_t>(regs.s - 1);
    m.mem[0x100 + regs.s] = static_cast<std::uint8_t>(back & 0xFF);
    regs.s = static_cast<std::uint8_t>(regs.s - 1);
}

void Load(SimRunner& run, const Compiled& compiled) {
    run.machine = std::make_unique<Machine>();
    run.io.clear();
    run.card = compiled.card_ram;
    run.bank2 = true;
    run.other_bank.assign(0x1000, 0);
    run.switch_at.assign(0x10000, false);
    for (std::uint16_t at : compiled.card_switches) {
        run.switch_at[at] = true;
    }
    InitMachine(*run.machine);
    LoadBytes(*run.machine, compiled.origin, compiled.image.data(), compiled.image.size());
    // the flat memory holds main ram and the language card with bank 2 at $D000 (the runner
    // keeps bank 1); aux data is out of reach here, as it is for code without lib/auxmem
    for (const MemorySegment& segment : compiled.segments) {
        if (segment.bank == MemoryBank::kCardBank1) {
            std::copy(segment.bytes.begin(), segment.bytes.end(),
                      run.other_bank.begin() + (segment.address - 0xD000));
        } else if (segment.bank != MemoryBank::kAux) {
            LoadBytes(*run.machine, segment.address, segment.bytes.data(), segment.bytes.size());
        }
    }
    IoHook hook;
    hook.base = 0xC000;
    hook.size = 0x1000;
    hook.read = IoRead;
    hook.write = IoWrite;
    hook.ctx = &run;
    SetIoHook(*run.machine, hook);
}

} // namespace

std::uint8_t IoReadValue(std::uint16_t address, std::size_t index) {
    return static_cast<std::uint8_t>((address * 7 + index * 13 + 0x5A) & 0xFF);
}

bool StartCompiled(SimRunner& run, const Compiled& compiled, std::string& error) {
    Load(run, compiled);
    Machine& m = *run.machine;
    Regs regs;
    regs.s = 0xFF;
    PushReturn(m, regs);
    regs.pc = compiled.origin;
    SetRegs(m, regs);
    RunOptions options;
    options.stop_pc = compiled.entry_ready;
    options.max_cycles = 10000000;
    RunResult result = Run(m, options);
    if (result.reason != StopReason::kStopAddress) {
        error = std::string("startup stopped: ") + StopReasonName(result.reason);
        return false;
    }
    return true;
}

SimCall CallCompiled(SimRunner& run, const Program& program, const Compiled& compiled,
                     std::int32_t function, const std::vector<std::uint16_t>& args,
                     std::uint64_t max_cycles) {
    Machine& m = *run.machine;
    const Function& fn = program.functions[static_cast<std::size_t>(function)];
    for (std::size_t i = 0; i < args.size() && i < fn.params.size(); i++) {
        std::int32_t param = fn.params[i];
        std::uint16_t at = compiled.layout.vars[static_cast<std::size_t>(param)].address;
        std::uint32_t size =
            TypeOf(program, program.vars[static_cast<std::size_t>(param)].type).size;
        m.mem[at] = static_cast<std::uint8_t>(args[i] & 0xFF);
        if (size == 2) {
            m.mem[static_cast<std::uint16_t>(at + 1)] = static_cast<std::uint8_t>(args[i] >> 8);
        }
    }
    Regs regs = GetRegs(m);
    PushReturn(m, regs);
    regs.pc = compiled.function_address[static_cast<std::size_t>(function)];
    SetRegs(m, regs);
    RunOptions options;
    options.stop_pc = kSentinel;
    options.max_cycles = max_cycles;
    RunResult result = Run(m, options);
    SimCall call;
    call.reason = result.reason;
    call.returned = result.reason == StopReason::kStopAddress;
    call.a = result.regs.a;
    call.x = result.regs.x;
    call.cycles = result.cycles;
    return call;
}

SimMain RunCompiledMain(SimRunner& run, const Compiled& compiled, std::uint64_t max_cycles) {
    Load(run, compiled);
    Machine& m = *run.machine;
    std::uint8_t pattern[256];
    for (int i = 0; i < 256; i++) {
        pattern[i] = static_cast<std::uint8_t>(i * 37 + 11);
        m.mem[i] = pattern[i];
    }
    Regs regs;
    regs.s = 0xFF;
    PushReturn(m, regs);
    regs.pc = compiled.origin;
    SetRegs(m, regs);
    SimMain out;
    RunOptions options;
    options.max_cycles = max_cycles;
    options.stop_pc = compiled.entry_ready;
    RunResult result = Run(m, options);
    out.total_cycles = result.cycles;
    if (result.reason != StopReason::kStopAddress) {
        out.reason = result.reason;
        return out;
    }
    options.stop_pc = compiled.after_main;
    result = Run(m, options);
    out.total_cycles += result.cycles;
    out.main_cycles = result.cycles;
    out.a = result.regs.a;
    out.x = result.regs.x;
    if (result.reason != StopReason::kStopAddress) {
        out.reason = result.reason;
        return out;
    }
    options.stop_pc = kSentinel;
    result = Run(m, options);
    out.total_cycles += result.cycles;
    out.reason = result.reason;
    out.finished = result.reason == StopReason::kStopAddress;
    out.zp_restored = true;
    for (int i = 0; i < 256; i++) {
        out.zp_restored = out.zp_restored && m.mem[i] == pattern[i];
    }
    return out;
}

} // namespace tisple
