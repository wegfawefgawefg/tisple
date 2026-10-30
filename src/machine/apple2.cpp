#include "machine/apple2.hpp"

#include "emu/disasm.hpp"
#include "machine/bus.hpp"
#include "machine/slots.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <new>
#include <type_traits>

namespace tisple {

static_assert(std::is_trivially_copyable_v<Apple2>, "a snapshot is a struct copy");

namespace {

constexpr uint32_t kSnapshotMagic = 0x32415054; // "TPA2"
constexpr uint32_t kSnapshotVersion = 5;        // 4: auxiliary memory; 5: the Mockingboard

struct SnapshotHeader {
    uint32_t magic = kSnapshotMagic;
    uint32_t version = kSnapshotVersion;
    uint64_t size = sizeof(Apple2);
};

// opcodes that stop a direct-loaded program: BRK and the undocumented ones
std::array<bool, 256> BuildStopTable() {
    std::array<bool, 256> table = {};
    for (int op = 0; op < 256; op++) {
        table[op] = op == 0x00 || !IsDocumentedOpcode(static_cast<uint8_t>(op));
    }
    return table;
}

const std::array<bool, 256> kStopsDirectLoad = BuildStopTable();

void Stop(Apple2& a, MachineStop reason, uint16_t pc, uint8_t opcode) {
    a.stopped = true;
    a.stop_reason = reason;
    a.stop_pc = pc;
    a.stop_opcode = opcode;
}

uint64_t NextKeyEvent(const Apple2& a) {
    uint64_t next = a.keys.key_down ? a.keys.release_cycle : UINT64_MAX;
    if (a.keys.queue_count > 0) {
        uint64_t queued = a.keys.queue[a.keys.queue_head].cycle;
        next = queued < next ? queued : next;
    }
    return next;
}

// video catch-up and scripted keys; returns the next cycle that needs a visit
uint64_t ServiceEvents(Apple2& a) {
    uint64_t now = a.cpu.cycles;
    if (now >= a.video.next_check) {
        CatchUpVideo(a, now);
    }
    Keyboard& k = a.keys;
    if (k.key_down && now >= k.release_cycle) {
        k.key_down = false;
        k.release_cycle = UINT64_MAX;
    }
    while (k.queue_count > 0 && k.queue[k.queue_head].cycle <= now) {
        PressKey(a, k.queue[k.queue_head].code);
        k.release_cycle = now + kScriptedKeyHoldCycles;
        k.queue_head = (k.queue_head + 1) % kMaxScheduledKeys;
        k.queue_count--;
    }
    uint64_t keys = NextKeyEvent(a);
    return keys < a.video.next_check ? keys : a.video.next_check;
}

// a pc at $C000 or above: the stop address, or rom space with nothing to run
bool CheckHighPc(Apple2& a, uint16_t pc) {
    if (pc == kStopAddress && a.direct_load) {
        Stop(a, MachineStop::kReturned, pc, 0);
        return true;
    }
    if (!CanExecute(a, pc)) {
        Stop(a, MachineStop::kNoRom, pc, 0);
        return true;
    }
    return false;
}

} // namespace

void PowerOn(Apple2& a) {
    // built in place: the struct is too big for a temporary on the stack
    std::memset(static_cast<void*>(&a), 0, sizeof(Apple2));
    new (&a) Apple2();
    InitMachine(a.cpu);
    InstallBus(a);
    ResetSoftSwitches(a);
    // a cleared text screen, as HOME leaves it
    std::memset(a.cpu.mem + 0x0400, 0xA0, 0x0400);
    a.video.next_check = 0;
    CaptureScreen(a, a.video.live);
    a.video.done = a.video.live;
}

std::unique_ptr<Apple2> NewApple2() {
    auto a = std::make_unique<Apple2>();
    PowerOn(*a);
    return a;
}

bool LoadRom(Apple2& a, const uint8_t* bytes, size_t size, std::string& error) {
    if (size != 0x3000 && size != 0x4000) {
        error = "a rom image is 12 KB ($D000-$FFFF) or 16 KB ($C000-$FFFF), not " +
                std::to_string(size) + " bytes";
        return false;
    }
    std::memset(a.rom.bytes, 0, sizeof(a.rom.bytes));
    std::memcpy(a.rom.bytes + (0x4000 - size), bytes, size);
    a.rom.present = true;
    a.rom.has_cx = size == 0x4000;
    return true;
}

bool LoadProgram(Apple2& a, const uint8_t* bytes, size_t size, uint16_t origin,
                 std::string& error) {
    if (size == 0) {
        error = "the program is empty";
        return false;
    }
    if (origin + size > 0xC000) {
        char text[96];
        std::snprintf(text, sizeof(text), "%zu bytes at $%04X run past $BFFF into i/o space", size,
                      origin);
        error = text;
        return false;
    }
    std::memcpy(a.cpu.mem + origin, bytes, size);
    // the entry's RTS pulls $FFFE and continues at $FFFF, where we stop
    a.cpu.mem[0x01FF] = kStopAddress >> 8;
    a.cpu.mem[0x01FE] = (kStopAddress - 1) & 0xFF;
    Regs regs;
    regs.pc = origin;
    regs.s = 0xFD;
    regs.p = kFlagU | kFlagI;
    SetRegs(a.cpu, regs);
    // $C082: rom (or the no-rom pattern) reads, writes to $D000-$FFFF ignored
    a.lc.bank2 = true;
    a.lc.read_ram = false;
    a.lc.write_enable = false;
    a.lc.prewrite = false;
    a.direct_load = true;
    a.stopped = false;
    a.stop_reason = MachineStop::kRunning;
    return true;
}

bool LoadSegment(Apple2& a, const MemorySegment& segment, std::string& error) {
    uint32_t start = segment.address;
    uint32_t end = start + static_cast<uint32_t>(segment.bytes.size());
    bool card = segment.bank == MemoryBank::kCard;
    uint32_t low = card ? 0xD000 : 0x0000;
    uint32_t high = card ? 0x10000 : 0xC000;
    if (start < low || end > high) {
        char text[128];
        std::snprintf(text, sizeof(text), "%zu bytes at $%04X do not fit %s memory",
                      segment.bytes.size(), segment.address, MemoryBankName(segment.bank));
        error = text;
        return false;
    }
    for (uint32_t addr = start; addr < end; addr++) {
        uint8_t value = segment.bytes[addr - start];
        if (segment.bank == MemoryBank::kMain) {
            a.cpu.mem[addr] = value;
        } else if (segment.bank == MemoryBank::kAux) {
            a.aux[addr] = value;
        } else if (addr >= 0xE000) {
            a.lc.e000[addr - 0xE000] = value;
        } else {
            a.lc.bank2_d000[addr - 0xD000] = value;
        }
    }
    return true;
}

uint8_t PeekAux(const Apple2& a, uint16_t addr) {
    if (addr < 0xC000) {
        return a.aux[addr];
    }
    if (addr >= 0xE000) {
        return a.lc.aux_e000[addr - 0xE000];
    }
    if (addr >= 0xD000) {
        return a.lc.aux_bank2_d000[addr - 0xD000];
    }
    return a.lc.aux_bank1_d000[addr - 0xC000];
}

void ResetApple2(Apple2& a) {
    ResetSoftSwitches(a);
    ResetCards(a);
    a.direct_load = false;
    a.stopped = false;
    a.stop_reason = MachineStop::kRunning;
    // the chip's own reset sequence: 7 cycles, then the vector at $FFFC
    m6502_desc_t desc = {};
    a.cpu.pins = m6502_init(&a.cpu.cpu, &desc);
    StepInstruction(a.cpu);
}

RunOutcome RunApple2(Apple2& a, uint64_t cycles) {
    return RunApple2Traced(a, cycles, nullptr, nullptr);
}

RunOutcome RunApple2Traced(Apple2& a, uint64_t cycles, MachineTraceFn trace, void* trace_ctx) {
    Machine& m = a.cpu;
    uint64_t start = m.cycles;
    uint64_t end = start + cycles;
    RunOutcome out;
    if (a.stopped) {
        // the cpu is done but the clock and the screen go on
        CatchUpVideo(a, end);
        m.cycles = end;
        out.reason = a.stop_reason;
        out.pc = a.stop_pc;
        out.cycles = cycles;
        return out;
    }
    uint64_t next = ServiceEvents(a);
    for (;;) {
        if (m.cycles >= next || m.cycles >= end) {
            if (m.cycles >= end) {
                break;
            }
            next = ServiceEvents(a);
        }
        uint16_t pc = M6502_GET_ADDR(m.pins);
        if (pc == a.return_pc && pc != 0) {
            Stop(a, MachineStop::kReturned, pc, 0);
            break;
        }
        uint8_t opcode = 0;
        if (pc < 0xC000) {
            // with auxiliary memory switched in, the fetch goes through the switches
            opcode = m.io.base == 0 ? PeekMemory(a, pc) : m.mem[pc];
        } else {
            if (CheckHighPc(a, pc)) {
                break;
            }
            opcode = PeekMemory(a, pc);
        }
        if (a.direct_load && kStopsDirectLoad[opcode]) {
            Stop(a, opcode == 0x00 ? MachineStop::kBrk : MachineStop::kIllegalOpcode, pc, opcode);
            break;
        }
        // the chip would hang there for good; the clock and the screen go on
        if (IsJamOpcode(opcode)) {
            Stop(a, MachineStop::kJammed, pc, opcode);
            break;
        }
        if (trace != nullptr) {
            trace(trace_ctx, m);
        }
        StepInstruction(m);
    }
    CatchUpVideo(a, m.cycles);
    out.reason = a.stopped ? a.stop_reason : MachineStop::kRunning;
    out.pc = M6502_GET_ADDR(m.pins);
    out.cycles = m.cycles - start;
    return out;
}

const char* MachineStopName(MachineStop reason) {
    switch (reason) {
    case MachineStop::kReturned:
        return "returned";
    case MachineStop::kBrk:
        return "BRK";
    case MachineStop::kIllegalOpcode:
        return "illegal opcode";
    case MachineStop::kNoRom:
        return "jumped into rom";
    case MachineStop::kJammed:
        return "jammed";
    default:
        return "running";
    }
}

std::string DescribeStop(const Apple2& a) {
    char text[128];
    switch (a.stop_reason) {
    case MachineStop::kReturned:
        return "returned from the entry point";
    case MachineStop::kBrk:
        std::snprintf(text, sizeof(text), "BRK at $%04X", a.stop_pc);
        return text;
    case MachineStop::kIllegalOpcode:
        std::snprintf(text, sizeof(text), "illegal opcode $%02X at $%04X", a.stop_opcode,
                      a.stop_pc);
        return text;
    case MachineStop::kJammed:
        std::snprintf(text, sizeof(text),
                      "the cpu jammed on opcode $%02X at $%04X (only a reset frees it)",
                      a.stop_opcode, a.stop_pc);
        return text;
    case MachineStop::kNoRom:
        if (a.stop_pc < 0xC100) {
            std::snprintf(text, sizeof(text), "jumped into i/o space at $%04X", a.stop_pc);
        } else if (a.stop_pc < 0xD000) {
            std::snprintf(text, sizeof(text), "jumped into empty slot rom at $%04X", a.stop_pc);
        } else {
            std::snprintf(text, sizeof(text), "jumped into ROM at $%04X (no rom loaded)",
                          a.stop_pc);
        }
        return text;
    default:
        return "running";
    }
}

uint64_t Cycles(const Apple2& a) {
    return a.cpu.cycles;
}

void PressKey(Apple2& a, uint8_t code) {
    a.keys.latch = static_cast<uint8_t>(code | 0x80);
    a.keys.key_down = true;
    a.keys.release_cycle = UINT64_MAX;
}

void ReleaseKey(Apple2& a) {
    a.keys.key_down = false;
    a.keys.release_cycle = UINT64_MAX;
}

bool ScheduleKey(Apple2& a, uint64_t cycle, uint8_t code) {
    Keyboard& k = a.keys;
    if (k.queue_count == kMaxScheduledKeys) {
        return false;
    }
    // keep the queue sorted by cycle: insert from the back
    uint32_t i = k.queue_count;
    while (i > 0) {
        const ScheduledKey& prev = k.queue[(k.queue_head + i - 1) % kMaxScheduledKeys];
        if (prev.cycle <= cycle) {
            break;
        }
        k.queue[(k.queue_head + i) % kMaxScheduledKeys] = prev;
        i--;
    }
    k.queue[(k.queue_head + i) % kMaxScheduledKeys] = {cycle, code};
    k.queue_count++;
    return true;
}

void SetPaddle(Apple2& a, int index, uint8_t value) {
    if (index >= 0 && index < 4) {
        a.game.paddle[index] = value;
    }
}

void SetButton(Apple2& a, int index, bool down) {
    if (index >= 0 && index < 3) {
        a.game.button[index] = down;
    }
}

uint64_t SpeakerToggleCycle(const Apple2& a, uint64_t n) {
    return a.speaker.toggles[n % kSpeakerRingSize];
}

std::vector<uint8_t> SaveSnapshot(const Apple2& a) {
    // a copy with the pointers zeroed: they mean nothing outside this process
    std::unique_ptr<Apple2> saved = std::make_unique<Apple2>();
    std::memcpy(static_cast<void*>(saved.get()), static_cast<const void*>(&a), sizeof(Apple2));
    saved->cpu.io = IoHook{};
    saved->cpu.bus_log = nullptr;
    m6502_snapshot_onsave(&saved->cpu.cpu);

    SnapshotHeader header;
    std::vector<uint8_t> bytes(sizeof(header));
    std::memcpy(bytes.data(), &header, sizeof(header));
    // zero runs as 0, then the run length in 7-bit groups (low first, top bit: more)
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(saved.get());
    size_t i = 0;
    while (i < sizeof(Apple2)) {
        if (raw[i] != 0) {
            bytes.push_back(raw[i]);
            i++;
            continue;
        }
        size_t run = 0;
        while (i + run < sizeof(Apple2) && raw[i + run] == 0) {
            run++;
        }
        i += run;
        bytes.push_back(0);
        do {
            uint8_t group = static_cast<uint8_t>(run & 0x7F);
            run >>= 7;
            bytes.push_back(static_cast<uint8_t>(group | (run != 0 ? 0x80 : 0)));
        } while (run != 0);
    }
    return bytes;
}

bool LoadSnapshot(Apple2& a, const uint8_t* bytes, size_t size, std::string& error) {
    SnapshotHeader header;
    if (size < sizeof(header)) {
        error = "snapshot is too short";
        return false;
    }
    std::memcpy(&header, bytes, sizeof(header));
    if (header.magic != kSnapshotMagic || header.version != kSnapshotVersion ||
        header.size != sizeof(Apple2)) {
        error = "not a snapshot from this build";
        return false;
    }
    std::unique_ptr<Apple2> loaded = std::make_unique<Apple2>();
    uint8_t* raw = reinterpret_cast<uint8_t*>(loaded.get());
    size_t out = 0;
    size_t i = sizeof(header);
    while (i < size) {
        uint8_t b = bytes[i++];
        if (b != 0) {
            if (out == sizeof(Apple2)) {
                break;
            }
            raw[out++] = b;
            continue;
        }
        size_t run = 0;
        int shift = 0;
        uint8_t group = 0x80;
        while ((group & 0x80) != 0 && i < size && shift < 63) {
            group = bytes[i++];
            run |= static_cast<size_t>(group & 0x7F) << shift;
            shift += 7;
        }
        if ((group & 0x80) != 0 || run > sizeof(Apple2) - out) {
            break;
        }
        std::memset(raw + out, 0, run);
        out += run;
    }
    if (out != sizeof(Apple2) || i != size) {
        error = "snapshot is damaged or the wrong size for this build";
        return false;
    }
    m6502_t live_cpu = a.cpu.cpu;
    std::memcpy(static_cast<void*>(&a), raw, sizeof(Apple2));
    m6502_snapshot_onload(&a.cpu.cpu, &live_cpu);
    a.cpu.bus_log = nullptr;
    InstallBus(a);
    return true;
}

} // namespace tisple
