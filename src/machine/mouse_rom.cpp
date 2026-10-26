// our own applemouse ii firmware, for the emulated card when no MouseInterface.rom is given.
// written for Tisple from Apple's documented interface (mouse.hpp); no code from Apple's rom.
//
// it is built for the slot it sits in, so it needs neither the X = $Cn and Y = $n0 that
// callers pass (they are the documented convention, and kept) nor the monitor rom: each
// routine moves the screen holes to or from the card's extra port (registers 8-13, see
// mouse.hpp) and the card does the rest at once. X and Y come back as they went in, as with
// Apple's; so does A (Apple's mostly gives 0, which nothing documents). C is clear, or set
// when SETMOUSE gets a mode above $0F and when SERVEMOUSE finds no interrupt of the mouse's
// (always: there are none). it uses no zero page and only the slot's own
// screen holes, plus the slot 0 holes CLAMPMOUSE reads. $Cn00 (PR#n, IN#n) and the Pascal
// entries only return with C set: the card has no text interface here.

#include "disk/code6502.hpp"
#include "machine/mouse.hpp"

#include <cstdio>
#include <cstring>

namespace tisple {

namespace {

// the slot's screen holes, by their slot 0 address
constexpr int kHoleXLow = 0x0478;
constexpr int kHoleYLow = 0x04F8;
constexpr int kHoleXHigh = 0x0578;
constexpr int kHoleYHigh = 0x05F8;
constexpr int kHoleStatus = 0x0778;
constexpr int kHoleMode = 0x07F8;

struct MouseRom {
    Code6502 c;
    int slot = 0;
    int command = 0; // the port's registers: $C088 + n * 16, then the data bytes
    int data = 0;
};

int Hole(const MouseRom& r, int base) {
    return base + r.slot;
}

// every routine ends here: A back, carry as it is
void Return(MouseRom& r) {
    r.c.Op(Mn::PLA);
    r.c.Op(Mn::RTS);
}

void EmitSet(MouseRom& r, std::int32_t at) {
    std::int32_t bad = r.c.Label("set.bad");
    r.c.Place(at);
    r.c.Op(Mn::PHA);
    r.c.Imm(Mn::CMP, 0x10);
    r.c.Br(Mn::BCS, bad);
    r.c.Mem(Mn::STA, Hole(r, kHoleMode));
    r.c.Mem(Mn::STA, r.command); // $00 | mode
    r.c.Place(bad);
    Return(r); // C from the compare: clear below $10
}

void EmitServe(MouseRom& r, std::int32_t at) {
    std::int32_t none = r.c.Label("serve.none");
    r.c.Place(at);
    r.c.Op(Mn::PHA);
    r.c.Imm(Mn::LDA, 0x20);
    r.c.Mem(Mn::STA, r.command);
    r.c.Mem(Mn::LDA, r.data);
    r.c.Imm(Mn::AND, 0x0E);
    r.c.Op(Mn::SEC);
    r.c.Br(Mn::BEQ, none);
    r.c.Mem(Mn::ORA, Hole(r, kHoleStatus));
    r.c.Mem(Mn::STA, Hole(r, kHoleStatus));
    r.c.Op(Mn::CLC);
    r.c.Place(none);
    Return(r);
}

void EmitRead(MouseRom& r, std::int32_t at) {
    const int holes[5] = {kHoleXLow, kHoleXHigh, kHoleYLow, kHoleYHigh, kHoleStatus};
    r.c.Place(at);
    r.c.Op(Mn::PHA);
    r.c.Imm(Mn::LDA, 0x10);
    r.c.Mem(Mn::STA, r.command);
    for (int i = 0; i < 5; i++) {
        r.c.Mem(Mn::LDA, r.data + i);
        r.c.Mem(Mn::STA, Hole(r, holes[i]));
    }
    r.c.Op(Mn::CLC);
    Return(r);
}

// CLEARMOUSE also zeroes the position holes, as Apple's firmware does
void EmitClear(MouseRom& r, std::int32_t at) {
    const int holes[4] = {kHoleXLow, kHoleXHigh, kHoleYLow, kHoleYHigh};
    r.c.Place(at);
    r.c.Op(Mn::PHA);
    r.c.Imm(Mn::LDA, 0x30);
    r.c.Mem(Mn::STA, r.command);
    r.c.Imm(Mn::LDA, 0);
    for (int i = 0; i < 4; i++) {
        r.c.Mem(Mn::STA, Hole(r, holes[i]));
    }
    r.c.Op(Mn::CLC);
    Return(r);
}

// the four bytes from holes at base + `slot` to the port, then the command
void EmitSend(MouseRom& r, std::int32_t at, int command, int slot) {
    const int holes[4] = {kHoleXLow, kHoleXHigh, kHoleYLow, kHoleYHigh};
    r.c.Place(at);
    r.c.Op(Mn::PHA);
    for (int i = 0; i < 4; i++) {
        r.c.Mem(Mn::LDA, holes[i] + slot);
        r.c.Mem(Mn::STA, r.data + i);
    }
    r.c.Imm(Mn::LDA, command);
    r.c.Mem(Mn::STA, r.command);
    r.c.Op(Mn::CLC);
    Return(r);
}

// CLAMPMOUSE: the bounds from the slot 0 holes in address order (low bound's low byte, high
// bound's low byte, low bound's high byte, high bound's high byte), A picks the axis
void EmitClamp(MouseRom& r, std::int32_t at) {
    const int holes[4] = {kHoleXLow, kHoleYLow, kHoleXHigh, kHoleYHigh};
    r.c.Place(at);
    r.c.Op(Mn::PHA);
    for (int i = 0; i < 4; i++) {
        r.c.Mem(Mn::LDA, holes[i]);
        r.c.Mem(Mn::STA, r.data + i);
    }
    r.c.Op(Mn::PLA);
    r.c.Op(Mn::PHA);
    r.c.Imm(Mn::AND, 0x01);
    r.c.Imm(Mn::ORA, 0x60);
    r.c.Mem(Mn::STA, r.command);
    r.c.Op(Mn::CLC);
    Return(r);
}

// HOMEMOUSE and INITMOUSE: just the command
void EmitCommand(MouseRom& r, std::int32_t at, int command) {
    r.c.Place(at);
    r.c.Op(Mn::PHA);
    r.c.Imm(Mn::LDA, command);
    r.c.Mem(Mn::STA, r.command);
    r.c.Op(Mn::CLC);
    Return(r);
}

void Pad(Code6502& c, int count) {
    for (int i = 0; i < count; i++) {
        c.Byte(0);
    }
}

} // namespace

void BuildMouseRom(int slot, uint8_t out[256]) {
    MouseRom r;
    r.slot = slot & 7;
    r.command = 0xC088 + r.slot * 16;
    r.data = r.command + 1;
    uint16_t base = static_cast<uint16_t>(0xC000 + r.slot * 0x100);
    Code6502& c = r.c;
    std::int32_t none = c.Label("none");
    std::int32_t entries[8];
    const char* names[8] = {"set", "serve", "read", "clear", "pos", "clamp", "home", "init"};
    for (int i = 0; i < 8; i++) {
        entries[i] = c.Label(names[i]);
    }
    // $Cn00: SEC, RTS; the identification bytes; the Pascal entries' offsets ($Cn0D-$Cn10)
    // and the mouse routines' ($Cn12-$Cn19)
    c.Place(none);
    c.Op(Mn::SEC);
    c.Op(Mn::RTS);
    Pad(c, 3);
    c.Byte(0x38); // $Cn05
    c.Byte(0x00);
    c.Byte(0x18); // $Cn07
    Pad(c, 3);
    c.Byte(0x01); // $Cn0B
    c.Byte(0x20); // $Cn0C: an X-Y pointing device
    for (int i = 0; i < 4; i++) {
        c.as.items.push_back(MakeByte(SymValue(none, 0, Part::Low)));
    }
    c.Byte(0x00); // $Cn11
    for (int i = 0; i < 8; i++) {
        c.as.items.push_back(MakeByte(SymValue(entries[i], 0, Part::Low)));
    }
    EmitSet(r, entries[0]);
    EmitServe(r, entries[1]);
    EmitRead(r, entries[2]);
    EmitClear(r, entries[3]);
    EmitSend(r, entries[4], 0x40, r.slot);
    EmitClamp(r, entries[5]);
    EmitCommand(r, entries[6], 0x70);
    EmitCommand(r, entries[7], 0x50);

    std::vector<uint8_t> bytes;
    std::string error;
    std::memset(out, 0, 256);
    if (!BuildCode(c, base, bytes, error) || bytes.size() > 0xFB) {
        // a programming error in this file; the tests catch it
        std::fprintf(stderr, "tisple: mouse firmware does not fit: %s\n", error.c_str());
        return;
    }
    std::memcpy(out, bytes.data(), bytes.size());
    out[0xFB] = 0xD6; // the mouse's id byte
}

} // namespace tisple
