// the applemouse ii card through its firmware entry points, with scripted motion: our own
// firmware always, the owner's MouseInterface.rom (with Apple2e.rom) when they are there.
// then programs/paint drawing a scripted stroke, checked pixel by pixel

#include "machine/mouse.hpp"
#include "test_backend.hpp"
#include "test_disk.hpp"
#include "test_machine.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>

namespace tisple_tests {

namespace {

constexpr int kSlot = tisple::kMouseSlot;
constexpr uint16_t kBase = 0xC000 + kSlot * 0x100;

enum Routine : int {
    kSet = 0,
    kServe,
    kRead,
    kClear,
    kPos,
    kClamp,
    kHome,
    kInit,
};

struct Call {
    bool returned = false;
    tisple::Regs regs;
    bool carry = false;
};

// LDA #value; LDX #$Cn; LDY #$n0; JSR routine; RTS at $6000, run to the end
Call CallMouse(tisple::Apple2& a, Routine routine, uint8_t value) {
    uint16_t entry = static_cast<uint16_t>(
        kBase + tisple::PeekMemory(a, static_cast<uint16_t>(kBase + 0x12 + routine)));
    std::vector<uint8_t> stub = {0xA9,
                                 value,
                                 0xA2,
                                 static_cast<uint8_t>(0xC0 + kSlot),
                                 0xA0,
                                 static_cast<uint8_t>(kSlot * 16),
                                 0x20,
                                 static_cast<uint8_t>(entry & 0xFF),
                                 static_cast<uint8_t>(entry >> 8),
                                 0x60};
    std::string error;
    tisple::LoadProgram(a, stub.data(), stub.size(), tisple::kDefaultOrigin, error);
    Call call;
    call.returned = RunToStop(a).reason == tisple::MachineStop::kReturned;
    call.regs = tisple::GetRegs(a.cpu);
    call.carry = (call.regs.p & 0x01) != 0;
    return call;
}

uint8_t Hole(const tisple::Apple2& a, uint16_t base) {
    return a.cpu.mem[base + kSlot];
}

int HoleX(const tisple::Apple2& a) {
    return static_cast<int16_t>(Hole(a, 0x0478) | (Hole(a, 0x0578) << 8));
}

int HoleY(const tisple::Apple2& a) {
    return static_cast<int16_t>(Hole(a, 0x04F8) | (Hole(a, 0x05F8) << 8));
}

// the bounds for CLAMPMOUSE in the slot 0 holes
void SetClampHoles(tisple::Apple2& a, int low, int high) {
    a.cpu.mem[0x0478] = static_cast<uint8_t>(low & 0xFF);
    a.cpu.mem[0x0578] = static_cast<uint8_t>((low >> 8) & 0xFF);
    a.cpu.mem[0x04F8] = static_cast<uint8_t>(high & 0xFF);
    a.cpu.mem[0x05F8] = static_cast<uint8_t>((high >> 8) & 0xFF);
}

void SetPosHoles(tisple::Apple2& a, int x, int y) {
    a.cpu.mem[0x0478 + kSlot] = static_cast<uint8_t>(x & 0xFF);
    a.cpu.mem[0x0578 + kSlot] = static_cast<uint8_t>((x >> 8) & 0xFF);
    a.cpu.mem[0x04F8 + kSlot] = static_cast<uint8_t>(y & 0xFF);
    a.cpu.mem[0x05F8 + kSlot] = static_cast<uint8_t>((y >> 8) & 0xFF);
}

struct Reading {
    bool ok = false;
    int x = 0;
    int y = 0;
    uint8_t status = 0;
};

Reading Read(tisple::Apple2& a) {
    Call call = CallMouse(a, kRead, 0);
    Reading r;
    r.ok = call.returned && !call.carry;
    r.x = HoleX(a);
    r.y = HoleY(a);
    r.status = Hole(a, 0x0778);
    return r;
}

bool Same(const Reading& r, int x, int y, uint8_t status, const char* what, const char* rom) {
    bool same = r.ok && r.x == x && r.y == y && r.status == status;
    if (!same) {
        std::fprintf(stderr, "mouse (%s) %s: x %d y %d status $%02X, want %d %d $%02X%s\n", rom,
                     what, r.x, r.y, r.status, x, y, status, r.ok ? "" : " (call failed)");
    }
    return same;
}

// the documented behaviour, step by step; rom names the firmware for messages
void TestFirmware(TestResults& results, tisple::Apple2& a, const char* rom) {
    // identification
    CHECK(results, tisple::PeekMemory(a, kBase + 0x05) == 0x38);
    CHECK(results, tisple::PeekMemory(a, kBase + 0x07) == 0x18);
    CHECK(results, tisple::PeekMemory(a, kBase + 0x0B) == 0x01);
    CHECK(results, tisple::PeekMemory(a, kBase + 0x0C) == 0x20);
    CHECK(results, tisple::PeekMemory(a, kBase + 0xFB) == 0xD6);

    Call call = CallMouse(a, kInit, 0);
    CHECK(results, call.returned && !call.carry);
    // hi-res: 0-279 by 0-191
    SetClampHoles(a, 0, 279);
    call = CallMouse(a, kClamp, 0);
    CHECK(results, call.returned && !call.carry);
    SetClampHoles(a, 0, 191);
    call = CallMouse(a, kClamp, 1);
    CHECK(results, call.returned && !call.carry);
    call = CallMouse(a, kSet, 1);
    CHECK(results, call.returned && !call.carry && a.cpu.mem[0x07F8 + kSlot] == 1);
    // the routines keep X and Y
    CHECK(results, call.regs.x == 0xC0 + kSlot && call.regs.y == kSlot * 16);

    uint64_t now = tisple::Cycles(a);
    tisple::ScheduleMouse(a, now, 100, 50, true);
    CHECK(results, Same(Read(a), 100, 50, 0xA0, "pressed and moved", rom));
    CHECK(results, Same(Read(a), 100, 50, 0xC0, "held", rom));
    // past the window: clamped
    tisple::ScheduleMouse(a, tisple::Cycles(a), 500, -5, false);
    CHECK(results, Same(Read(a), 279, 0, 0x60, "released past the window", rom));
    // a move in the future waits
    tisple::ScheduleMouse(a, tisple::Cycles(a) + 100000, 7, 8, false);
    CHECK(results, Same(Read(a), 279, 0, 0x00, "before the scripted move", rom));
    tisple::RunApple2(a, 100000);
    CHECK(results, Same(Read(a), 7, 8, 0x20, "after it", rom));

    SetPosHoles(a, 200, 100);
    call = CallMouse(a, kPos, 0);
    CHECK(results, call.returned && !call.carry);
    SetPosHoles(a, 0, 0);
    CHECK(results, Same(Read(a), 200, 100, 0x00, "POSMOUSE", rom));
    // the window's corner
    SetClampHoles(a, 10, 191);
    CallMouse(a, kClamp, 1);
    call = CallMouse(a, kHome, 0);
    CHECK(results, call.returned && !call.carry);
    CHECK(results, Same(Read(a), 0, 10, 0x00, "HOMEMOUSE", rom));
    tisple::ScheduleMouse(a, tisple::Cycles(a), 30, 40, false);
    CHECK(results, Same(Read(a), 30, 40, 0x20, "moved again", rom));
    // CLEARMOUSE zeroes the position holes too, and leaves the status
    a.cpu.mem[0x0778 + kSlot] = 0x11;
    call = CallMouse(a, kClear, 0);
    CHECK(results, call.returned && !call.carry);
    CHECK(results, HoleX(a) == 0 && HoleY(a) == 0 && Hole(a, 0x0778) == 0x11);
    CHECK(results, Same(Read(a), 0, 0, 0x00, "CLEARMOUSE", rom));

    // no interrupts: SERVEMOUSE always says it was not the mouse
    call = CallMouse(a, kServe, 0);
    CHECK(results, call.returned && call.carry);
    // a mode past $0F is refused
    call = CallMouse(a, kSet, 0x10);
    CHECK(results, call.returned && call.carry && a.cpu.mem[0x07F8 + kSlot] == 1);
}

void TestOurFirmware(TestResults& results) {
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    tisple::InsertMouseCard(*a, kSlot, nullptr);
    CHECK(results, tisple::HasMouseCard(*a));
    TestFirmware(results, *a, "our firmware");
    CHECK(results, tisple::DescribeMouse(*a).find("mouse: slot 4, x 0 y 0") == 0);
    // with no program touching it, nothing to say
    std::unique_ptr<tisple::Apple2> idle = tisple::NewApple2();
    tisple::InsertMouseCard(*idle, kSlot, nullptr);
    CHECK(results, tisple::DescribeMouse(*idle).empty());
}

void TestRealFirmware(TestResults& results) {
    std::vector<uint8_t> mouse_rom = OwnerRom("MouseInterface.rom");
    std::vector<uint8_t> main_rom = OwnerRom("Apple2e.rom");
    if (mouse_rom.size() != tisple::kMouseRomBytes || main_rom.empty()) {
        std::printf("mouse: MouseInterface.rom or Apple2e.rom not found, skipping the real "
                    "firmware\n");
        return;
    }
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    std::string error;
    CHECK(results, tisple::LoadRom(*a, main_rom.data(), main_rom.size(), error));
    tisple::InsertMouseCard(*a, kSlot, mouse_rom.data());
    TestFirmware(results, *a, "MouseInterface.rom");
}

// ---- programs/paint

using Pixels = std::set<std::pair<int, int>>;

bool PixelOn(const tisple::Apple2& a, int x, int y) {
    uint16_t row =
        static_cast<uint16_t>(0x2000 + (y & 7) * 0x400 + ((y >> 3) & 7) * 0x80 + (y >> 6) * 40);
    return (a.cpu.mem[row + x / 7] & (1 << (x % 7))) != 0;
}

Pixels OnPixels(const tisple::Apple2& a) {
    Pixels on;
    for (int y = 0; y < 192; y++) {
        for (int x = 0; x < 280; x++) {
            if (PixelOn(a, x, y)) {
                on.insert({x, y});
            }
        }
    }
    return on;
}

// the line hgr.line draws (Bresenham, both ends)
void AddLine(Pixels& out, int x0, int y0, int x1, int y1) {
    int dx = std::abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        out.insert({x0, y0});
        if (x0 == x1 && y0 == y1) {
            return;
        }
        int twice = 2 * err;
        if (twice >= dy) {
            err += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void AddCursor(Pixels& out, int x, int y) {
    out.insert({x, y});
    out.insert({x - 2, y});
    out.insert({x + 2, y});
    out.insert({x, y - 2});
    out.insert({x, y + 2});
}

bool SamePixels(const Pixels& got, const Pixels& want, const char* what) {
    if (got == want) {
        return true;
    }
    std::fprintf(stderr, "paint %s: %zu pixels on, want %zu\n", what, got.size(), want.size());
    for (const auto& p : want) {
        if (got.count(p) == 0) {
            std::fprintf(stderr, "  missing %d,%d\n", p.first, p.second);
        }
    }
    for (const auto& p : got) {
        if (want.count(p) == 0) {
            std::fprintf(stderr, "  extra %d,%d\n", p.first, p.second);
        }
    }
    return false;
}

void TestPaint(TestResults& results) {
    Compiled c = CompileFile("programs/paint/main.tsp");
    CHECK(results, c.ok);
    if (!c.ok) {
        std::fprintf(stderr, "%s", c.errors.c_str());
        return;
    }
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    tisple::InsertMouseCard(*a, kSlot, nullptr);
    std::string error;
    CHECK(results,
          tisple::LoadProgram(*a, c.code.image.data(), c.code.image.size(), c.code.origin, error));
    const uint64_t f = tisple::kCyclesPerFrame;
    // press at 20,20, drag through 60,40 to 100,60, let go, move away
    tisple::ScheduleMouse(*a, 10 * f, 20, 20, true);
    tisple::ScheduleMouse(*a, 14 * f, 60, 40, true);
    tisple::ScheduleMouse(*a, 18 * f, 100, 60, true);
    tisple::ScheduleMouse(*a, 22 * f, 100, 60, false);
    tisple::ScheduleMouse(*a, 26 * f, 200, 150, false);
    // past the window's edge: drawn to the edge
    tisple::ScheduleMouse(*a, 30 * f, 250, 100, true);
    tisple::ScheduleMouse(*a, 34 * f, 400, 100, true);
    tisple::ScheduleMouse(*a, 38 * f, 270, 10, false);
    tisple::ScheduleKey(*a, 46 * f, 'C');
    tisple::ScheduleKey(*a, 60 * f, 0x1B);

    tisple::RunApple2(*a, 42 * f);
    Pixels want;
    AddLine(want, 20, 20, 60, 40);
    AddLine(want, 60, 40, 100, 60);
    AddLine(want, 250, 100, 279, 100);
    AddCursor(want, 270, 10);
    CHECK(results, !a->stopped && a->sw.hires && !a->sw.text);
    CHECK(results, SamePixels(OnPixels(*a), want, "strokes"));
    // a key clears (6 frames for the page); the cursor stays
    tisple::RunApple2(*a, 16 * f);
    want.clear();
    AddCursor(want, 270, 10);
    CHECK(results, SamePixels(OnPixels(*a), want, "after a key"));
    // Esc quits, back to text
    tisple::RunApple2(*a, 10 * f);
    CHECK(results, a->stopped && a->stop_reason == tisple::MachineStop::kReturned);
    CHECK(results, a->sw.text);
}

} // namespace

void RunMachineMouseTests(TestResults& results) {
    TestOurFirmware(results);
    TestRealFirmware(results);
    TestPaint(results);
}

} // namespace tisple_tests
