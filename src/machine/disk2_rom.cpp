// our own disk ii boot rom, for the emulated card when no DISK2.rom is given. written for
// Tisple from the disk format and the boot interface below; no code from Apple's rom.
//
// what a disk sees (the interface boot sectors rely on, as with Apple's 16-sector rom):
// - $Cn00: cold boot. turns the motor on (drive 1, read mode), walks the head to track 0,
//   then reads physical sectors 0, 1, ... of track 0 to $0800, $0900, ... while the next
//   sector number is below the byte at $0800 (one sector when it is 0 or 1), and jumps to
//   $0801 with X = slot * 16.
// - $Cn5C: read one more sector. in: $26/$27 the buffer, $3D the physical sector, $41 the
//   track the head is on (the rom never steps here). it reads that sector to ($26), adds 1
//   to $27 and to $3D, then reads again while $3D is below ($0800), else jumps to $0801 with
//   X = slot * 16. this is how DOS 3.3's first sector loads the rest of its boot.
// - zero page on entry to $0801: $2B = slot * 16, $26 = 0, $27 = the page after the last
//   one read, $3D = the next sector, $41 = 0, $40 = the track found (0); $3C is scratch.
//   A = $3D, Y = 0. $3E/$3F are never touched (boot sectors keep $Cn5C there).
// - $0300-$0355 hold scratch from the last sector; $0356-$03D5 the nibble table: the byte
//   at $02D6 + n is the 6-bit value of disk nibble n.
// - signature: $Cn01 = $20, $Cn03 = $00, $Cn05 = $03, $Cn07 = $3C (the autostart rom's
//   test for a disk controller) and $CnFF = $00 (16 sectors, which ProDOS checks).
// - unlike Apple's rom it uses no monitor routines and waits for nothing: the emulated
//   stepper moves at once. so it is for the emulator only; real cards keep their own rom.

#include "disk/code6502.hpp"
#include "machine/disk2.hpp"

#include <cstdio>
#include <cstring>

namespace tisple {

namespace {

constexpr int kReadEntry = 0x5C;
constexpr int kTable = 0x02D6; // + nibble
constexpr int kScratch = 0x0300;

// $02D6 + n = n's 6-bit value for each of the 64 disk nibbles: a nibble has its top bit
// set, two neighbouring ones among bits 0-6, and at most one pair of neighbouring zeros.
// on entry Y = 0
void EmitNibbleTable(Code6502& c) {
    std::int32_t next = c.Label("table.next");
    std::int32_t skip = c.Label("table.skip");
    c.Imm(Mn::LDX, 0x80);
    c.Place(next);
    c.Mem(Mn::STX, 0x3C);
    c.Op(Mn::TXA);
    c.OpA(Mn::ASL); // carry = bit 7 = 1 from here to the SBC
    c.Mem(Mn::AND, 0x3C);
    c.Imm(Mn::AND, 0x7E); // bit i: bits i and i-1 are both ones
    c.Br(Mn::BEQ, skip);
    c.Op(Mn::TXA);
    c.OpA(Mn::ASL);
    c.Mem(Mn::ORA, 0x3C);
    c.Imm(Mn::EOR, 0xFF);
    c.Imm(Mn::AND, 0x7E); // bit i: bits i and i-1 are both zeros
    c.Mem(Mn::STA, 0x3C);
    c.Imm(Mn::SBC, 1);
    c.Mem(Mn::AND, 0x3C); // zero unless two such pairs
    c.Br(Mn::BNE, skip);
    c.Op(Mn::TYA);
    c.MemX(Mn::STA, kTable);
    c.Op(Mn::INY);
    c.Place(skip);
    c.Op(Mn::INX);
    c.Br(Mn::BNE, next);
}

void EmitColdStart(Code6502& c, int slot16) {
    // the signature bytes are these instructions' operands
    c.Imm(Mn::LDX, 0x20);
    c.Imm(Mn::LDY, 0x00);
    c.Imm(Mn::LDX, 0x03);
    c.Imm(Mn::LDX, 0x3C);
    EmitNibbleTable(c);
    c.Mem(Mn::STX, 0x26); // X = 0
    c.Mem(Mn::STX, 0x3D);
    c.Mem(Mn::STX, 0x41);
    c.Imm(Mn::LDA, 0x08);
    c.Mem(Mn::STA, 0x27);
    c.Imm(Mn::LDX, slot16);
    c.Mem(Mn::STX, 0x2B);
    c.MemX(Mn::LDA, 0xC08E); // read mode
    c.MemX(Mn::LDA, 0xC08A); // drive 1
    c.MemX(Mn::LDA, 0xC089); // motor on
    // walk the head down 80 half-tracks: each phase pulls it one step toward track 0
    std::int32_t step = c.Label("cold.step");
    c.Imm(Mn::LDY, kDiskHalfTracks - 1);
    c.Place(step);
    c.Op(Mn::TYA);
    c.Imm(Mn::AND, 3);
    c.OpA(Mn::ASL);
    c.Imm(Mn::ORA, slot16);
    c.Op(Mn::TAX);
    c.MemX(Mn::LDA, 0xC081); // phase on
    c.MemX(Mn::LDA, 0xC080); // phase off
    c.Op(Mn::DEY);
    c.Br(Mn::BPL, step);
}

// $Cn5C on. latch is the data register's address for this slot
void EmitRead(Code6502& c, int slot16, int latch, std::int32_t read) {
    std::int32_t prologue = c.Label("read.prologue");
    std::int32_t pair = c.Label("read.pair");
    std::int32_t aux = c.Label("read.aux");
    std::int32_t data = c.Label("read.data");
    std::int32_t sum = c.Label("read.sum");
    std::int32_t fix = c.Label("read.fix");
    std::int32_t fix_next = c.Label("read.fix_next");
    c.Place(read);
    c.Jump(Mn::JSR, prologue);
    c.Imm(Mn::CPX, 0x96);
    c.Br(Mn::BNE, read);
    c.Jump(Mn::JSR, pair); // volume
    c.Jump(Mn::JSR, pair); // track
    c.Mem(Mn::STA, 0x40);
    c.Jump(Mn::JSR, pair); // sector
    c.Mem(Mn::CMP, 0x3D);
    c.Br(Mn::BNE, read);
    c.Mem(Mn::LDA, 0x40);
    c.Mem(Mn::CMP, 0x41);
    c.Br(Mn::BNE, read);
    c.Jump(Mn::JSR, prologue);
    c.Imm(Mn::CPX, 0xAD);
    c.Br(Mn::BNE, read);
    // 86 nibbles of low bits to the scratch page, 256 of high bits to the buffer, then the
    // checksum; each nibble's value is xor the one before, so A runs the chain
    c.Imm(Mn::LDY, 0);
    c.Op(Mn::TYA);
    c.Place(aux);
    c.Mem(Mn::LDX, latch);
    c.Br(Mn::BPL, aux);
    c.MemX(Mn::EOR, kTable);
    c.MemY(Mn::STA, kScratch);
    c.Op(Mn::INY);
    c.Imm(Mn::CPY, 86);
    c.Br(Mn::BNE, aux);
    c.Imm(Mn::LDY, 0);
    c.Place(data);
    c.Mem(Mn::LDX, latch);
    c.Br(Mn::BPL, data);
    c.MemX(Mn::EOR, kTable);
    c.IndY(Mn::STA, 0x26);
    c.Op(Mn::INY);
    c.Br(Mn::BNE, data);
    c.Place(sum);
    c.Mem(Mn::LDX, latch);
    c.Br(Mn::BPL, sum);
    c.MemX(Mn::EOR, kTable);
    c.Br(Mn::BNE, read);
    // byte i takes its two low bits from scratch entry i mod 86, swapped
    c.Imm(Mn::LDX, 0);
    c.Place(fix);
    c.IndY(Mn::LDA, 0x26);
    c.MemX(Mn::LSR, kScratch);
    c.OpA(Mn::ROL);
    c.MemX(Mn::LSR, kScratch);
    c.OpA(Mn::ROL);
    c.IndY(Mn::STA, 0x26);
    c.Op(Mn::INX);
    c.Imm(Mn::CPX, 86);
    c.Br(Mn::BCC, fix_next);
    c.Imm(Mn::LDX, 0);
    c.Place(fix_next);
    c.Op(Mn::INY);
    c.Br(Mn::BNE, fix);
    c.Mem(Mn::INC, 0x27);
    c.Mem(Mn::INC, 0x3D);
    c.Mem(Mn::LDA, 0x3D);
    c.Mem(Mn::CMP, 0x0800);
    c.Br(Mn::BCC, read);
    c.Imm(Mn::LDX, slot16);
    c.Mem(Mn::JMP, 0x0801);

    // D5 AA, then the third nibble in X
    std::int32_t again = c.Label("read.again");
    std::int32_t nibble = c.Label("read.nibble");
    c.Place(prologue);
    c.Jump(Mn::JSR, nibble);
    c.Place(again);
    c.Imm(Mn::CPX, 0xD5);
    c.Br(Mn::BNE, prologue);
    c.Jump(Mn::JSR, nibble);
    c.Imm(Mn::CPX, 0xAA);
    c.Br(Mn::BNE, again);
    c.Place(nibble);
    c.Mem(Mn::LDX, latch);
    c.Br(Mn::BPL, nibble);
    c.Op(Mn::RTS);

    // a 4-and-4 byte: odd bits then even bits, each with ones between; inline reads keep it
    // quick enough for the next pair
    std::int32_t second = c.Label("read.second");
    c.Place(pair);
    c.Mem(Mn::LDX, latch);
    c.Br(Mn::BPL, pair);
    c.Op(Mn::TXA);
    c.Op(Mn::SEC);
    c.OpA(Mn::ROL);
    c.Mem(Mn::STA, 0x3C);
    c.Place(second);
    c.Mem(Mn::LDX, latch);
    c.Br(Mn::BPL, second);
    c.Op(Mn::TXA);
    c.Mem(Mn::AND, 0x3C);
    c.Op(Mn::RTS);
}

} // namespace

void BuildDisk2BootRom(int slot, uint8_t out[256]) {
    int slot16 = (slot & 7) * 16;
    uint16_t base = static_cast<uint16_t>(0xC000 + (slot & 7) * 0x100);
    Code6502 c;
    EmitColdStart(c, slot16);
    std::int32_t read = c.Label("read");
    // pad to the fixed $Cn5C entry
    std::vector<uint8_t> cold;
    std::string error;
    Code6502 probe = c;
    BuildCode(probe, base, cold, error);
    for (size_t i = cold.size(); i < kReadEntry; i++) {
        c.Op(Mn::NOP);
    }
    EmitRead(c, slot16, 0xC08C + slot16, read);
    std::vector<uint8_t> bytes;
    std::memset(out, 0, 256);
    if (cold.size() > kReadEntry || !BuildCode(c, base, bytes, error) || bytes.size() > 255 ||
        LabelAddress(c, read) != base + kReadEntry) {
        // a programming error in this file; the tests catch it
        std::fprintf(stderr, "tisple: disk ii boot rom does not fit: %s\n", error.c_str());
        return;
    }
    std::memcpy(out, bytes.data(), bytes.size()); // $CnFF stays $00
}

} // namespace tisple
