// lib/save's writer: a sector's nibbles worked out, the address field it writes after, and the
// data field written the way DOS 3.3's RWTS writes one. License: MIT as well as GPL-3.0-or-later (disk_writer.hpp).
// Original to Tisple; the timing (a nibble every 32 cycles, self-sync bytes 40) is the Disk
// II's, as Beneath Apple DOS and the Apple II Reference Manual give it.

#include "disk/writer_code.hpp"

namespace tisple {

namespace {

// nibbles: about three turns, so a drive that is still coming up to speed has time
constexpr int kAddressBudget = 0x5000;

// one nibble out: A to the data register, then shifting (STA Q6H,X loads on its last cycle;
// ORA Q6L,X is 4 more)
void Load(Code6502& c) {
    c.MemX(Mn::STA, 0xC08D);
    c.MemX(Mn::ORA, 0xC08C);
}

// a fixed byte, 32 cycles after the load before it (whose ORA is counted)
void Fixed(Code6502& c, int value) {
    EmitPad(c, 21);
    c.Imm(Mn::LDA, value);
    Load(c);
}

} // namespace

// the piece's 343 nibbles: the 86 low entries (bytes k, k + 86, k + 172, each pair swapped)
// to Low(), the 256 high six bits to High(), each as the nibble of its value xor the one
// before, and the last value's nibble to Sum(). The four loads of the piece's bytes are
// patched by next
void EmitWriterPrenibble(Writer& w, const WriterLabels& at) {
    Code6502& c = w.l.c;
    std::int32_t pass0 = c.Label("pre.pass0");
    std::int32_t pass1 = c.Label("pre.pass1");
    std::int32_t pass2 = c.Label("pre.pass2");
    std::int32_t chain_low = c.Label("pre.chain_low");
    std::int32_t chain_high = c.Label("pre.chain_high");
    int prev = w.Var(kWvPrev);
    c.Place(at.prenibble);
    c.Imm(Mn::LDY, 0);
    c.Place(pass0);
    c.Place(at.source[0]);
    c.MemY(Mn::LDA, 0x0300);
    c.Imm(Mn::AND, 3);
    c.Op(Mn::TAX);
    c.MemLabelX(Mn::LDA, at.swaps, 0);
    c.MemY(Mn::STA, w.Low());
    c.Op(Mn::INY);
    c.Imm(Mn::CPY, 86);
    c.Br(Mn::BNE, pass0);
    c.Place(pass1);
    c.Place(at.source[1]);
    c.MemY(Mn::LDA, 0x0300);
    c.Imm(Mn::AND, 3);
    c.Op(Mn::TAX);
    c.MemLabelX(Mn::LDA, at.swaps, 4);
    c.MemY(Mn::ORA, w.Low() - 86);
    c.MemY(Mn::STA, w.Low() - 86);
    c.Op(Mn::INY);
    c.Imm(Mn::CPY, 172);
    c.Br(Mn::BNE, pass1);
    c.Place(pass2);
    c.Place(at.source[2]);
    c.MemY(Mn::LDA, 0x0300);
    c.Imm(Mn::AND, 3);
    c.Op(Mn::TAX);
    c.MemLabelX(Mn::LDA, at.swaps, 8);
    c.MemY(Mn::ORA, w.Low() - 172);
    c.MemY(Mn::STA, w.Low() - 172);
    c.Op(Mn::INY);
    c.Br(Mn::BNE, pass2);
    c.Imm(Mn::LDA, 0);
    c.Mem(Mn::STA, prev);
    c.Place(chain_low);
    c.MemY(Mn::LDA, w.Low());
    c.Op(Mn::PHA);
    c.Mem(Mn::EOR, prev);
    c.Op(Mn::TAX);
    c.MemLabelX(Mn::LDA, at.nibbles, 0);
    c.MemY(Mn::STA, w.Low());
    c.Op(Mn::PLA);
    c.Mem(Mn::STA, prev);
    c.Op(Mn::INY);
    c.Imm(Mn::CPY, 86);
    c.Br(Mn::BNE, chain_low);
    c.Imm(Mn::LDY, 0);
    // the high values: byte i's top six bits xor byte i - 1's are (b[i] xor b[i - 1]) >> 2, so
    // two loads and an xor a byte; byte 0's against the last low entry, byte 255's alone
    c.Place(at.source[3]);
    c.Mem(Mn::LDA, 0x0300);
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.Mem(Mn::EOR, prev);
    c.Op(Mn::TAX);
    c.MemLabelX(Mn::LDA, at.nibbles, 0);
    c.Mem(Mn::STA, w.High());
    c.Imm(Mn::LDY, 1);
    c.Place(chain_high);
    c.Place(at.source[4]);
    c.MemY(Mn::LDA, 0x0300);
    c.Place(at.source_back);
    c.MemY(Mn::EOR, 0x0300); // patched: the bytes less one
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.Op(Mn::TAX);
    c.MemLabelX(Mn::LDA, at.nibbles, 0);
    c.MemY(Mn::STA, w.High());
    c.Op(Mn::INY);
    c.Br(Mn::BNE, chain_high); // 28 cycles a byte
    c.Op(Mn::DEY);
    c.Place(at.source[5]);
    c.MemY(Mn::LDA, 0x0300);
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.Op(Mn::TAX);
    c.MemLabelX(Mn::LDA, at.nibbles, 0);
    c.Mem(Mn::STA, w.Sum());
    c.Op(Mn::RTS);
}

void EmitPad(Code6502& c, int cycles) {
    if (cycles % 2 == 1) {
        std::int32_t on = c.Label("pad");
        c.Jump(Mn::JMP, on);
        c.Place(on);
        cycles -= 3;
    }
    for (int i = 0; i < cycles; i += 2) {
        c.Op(Mn::NOP);
    }
}

// the next good address field to the field bytes, with its epilogue's DE AA; carry clear with
// interrupts off (SEI from its first mark on), or carry set after kAddressBudget nibbles with
// none, the I flag the caller's. As lib/disk's reader finds one (reader_read.cpp), but for the
// epilogue: a data field written next starts from the EB on, where the gap begins
void EmitWriterAddress(Writer& w, std::int32_t address) {
    Code6502& c = w.l.c;
    std::int32_t retry = c.Label("waddr.retry");
    std::int32_t look = c.Label("waddr.look");
    std::int32_t check = c.Label("waddr.check");
    std::int32_t aa = c.Label("waddr.aa");
    std::int32_t mark = c.Label("waddr.96");
    std::int32_t odd = c.Label("waddr.odd");
    std::int32_t even = c.Label("waddr.even");
    std::int32_t de = c.Label("waddr.de");
    std::int32_t epilogue_aa = c.Label("waddr.epilogue_aa");
    std::int32_t timeout = c.Label("waddr.timeout");
    c.Place(address);
    c.Imm(Mn::LDA, kAddressBudget & 0xFF);
    c.Mem(Mn::STA, w.Var(kWvCountLo));
    c.Imm(Mn::LDA, kAddressBudget >> 8);
    c.Mem(Mn::STA, w.Var(kWvCountHi));
    c.Br(Mn::BNE, look);
    c.Place(retry);
    w.RestoreI();
    c.Place(look);
    w.l.ReadLatch(look);
    c.Mem(Mn::DEC, w.Var(kWvCountLo));
    c.Br(Mn::BNE, check);
    c.Mem(Mn::DEC, w.Var(kWvCountHi));
    c.Br(Mn::BEQ, timeout);
    c.Place(check);
    c.Imm(Mn::CPX, 0xD5);
    c.Br(Mn::BNE, look);
    c.Op(Mn::SEI);
    c.Place(aa);
    w.l.ReadLatch(aa);
    c.Imm(Mn::CPX, 0xAA);
    c.Br(Mn::BNE, retry);
    c.Place(mark);
    w.l.ReadLatch(mark);
    c.Imm(Mn::CPX, 0x96);
    c.Br(Mn::BNE, retry);
    c.Imm(Mn::LDY, 3);
    c.Place(odd);
    w.l.ReadLatch(odd);
    c.Op(Mn::TXA);
    c.Op(Mn::SEC);
    c.OpA(Mn::ROL);
    c.Mem(Mn::STA, w.Var(kWvTemp));
    c.Place(even);
    w.l.ReadLatch(even);
    c.Op(Mn::TXA);
    c.Mem(Mn::AND, w.Var(kWvTemp));
    c.MemY(Mn::STA, w.Var(kWvField));
    c.Op(Mn::DEY);
    c.Br(Mn::BPL, odd);
    // the epilogue's DE AA straight away (a nibble every 32 cycles), then the checksum
    c.Place(de);
    w.l.ReadLatch(de);
    c.Imm(Mn::CPX, 0xDE);
    c.Br(Mn::BNE, retry);
    c.Place(epilogue_aa);
    w.l.ReadLatch(epilogue_aa);
    c.Imm(Mn::CPX, 0xAA);
    c.Br(Mn::BNE, retry);
    c.Mem(Mn::LDA, w.Var(kWvField));
    c.Mem(Mn::EOR, w.Var(kWvField + 1));
    c.Mem(Mn::EOR, w.Var(kWvField + 2));
    c.Mem(Mn::EOR, w.Var(kWvField + 3));
    c.Br(Mn::BNE, retry);
    c.Op(Mn::CLC);
    c.Op(Mn::RTS);
    c.Place(timeout);
    c.Op(Mn::SEC);
    c.Op(Mn::RTS);
}

// right after an address field and its DE AA, from the EB on, where the gap before the data
// field begins: write mode with $FF loaded, four more self-sync bytes 40 cycles apart, D5 AA
// AD, the 86 low nibbles and the 256 high ones and the checksum, DE AA EB and an $FF, 32
// cycles apart; then read mode. Interrupts are off. Uses A, X, Y
void EmitWriterField(Writer& w, std::int32_t field) {
    Code6502& c = w.l.c;
    std::int32_t sync = c.Label("field.sync");
    std::int32_t low = c.Label("field.low");
    std::int32_t low_out = c.Label("field.low_out");
    std::int32_t high = c.Label("field.high");
    std::int32_t high_out = c.Label("field.high_out");
    c.Place(field);
    c.Mem(Mn::LDX, w.Var(kWvSlot));
    c.MemX(Mn::LDA, 0xC08D); // Q6 on
    c.MemX(Mn::LDA, 0xC08E); // Q7 off
    c.Imm(Mn::LDA, 0xFF);
    c.MemX(Mn::STA, 0xC08F); // Q7 on: write mode, $FF loaded (the clock starts here)
    c.MemX(Mn::ORA, 0xC08C); // 4
    c.Imm(Mn::LDY, 4);       // 2
    EmitPad(c, 3);           // the first sync's 40: 4 + 2 + 3 + 24 + 2 + 5
    c.Place(sync);
    EmitPad(c, 24);
    c.Imm(Mn::LDA, 0xFF);
    Load(c);
    c.Op(Mn::DEY);         // 2
    c.Br(Mn::BNE, sync);   // 3: 4 + 2 + 3 + 24 + 2 + 5 = 40
    EmitPad(c, 25);        // the last sync's 40: 4 + 2 + 2 + 25 + 2 + 5
    c.Imm(Mn::LDA, 0xD5);
    Load(c);
    Fixed(c, 0xAA);
    Fixed(c, 0xAD);
    c.Imm(Mn::LDY, 170); // 2: the low nibbles indexed 170-255, so Y is 0 after
    EmitPad(c, 17);      // 4 + 2 + 17 + 4 + 5
    c.Place(low);
    c.MemY(Mn::LDA, w.LowBase()); // 4, no page crossed
    Load(c);
    c.Op(Mn::INY);         // 2
    c.Br(Mn::BEQ, low_out); // 2 / 3
    EmitPad(c, 12);
    c.Jump(Mn::JMP, low);  // 3: 4 + 2 + 2 + 12 + 3 + 4 + 5 = 32
    c.Place(low_out);
    EmitPad(c, 14); // 4 + 2 + 3 + 14 + 4 + 5
    c.Place(high);
    c.MemY(Mn::LDA, w.High());
    Load(c);
    c.Op(Mn::INY);
    c.Br(Mn::BEQ, high_out);
    EmitPad(c, 12);
    c.Jump(Mn::JMP, high);
    c.Place(high_out);
    EmitPad(c, 14);
    c.Mem(Mn::LDA, w.Sum()); // 4, absolute
    Load(c);
    Fixed(c, 0xDE);
    Fixed(c, 0xAA);
    Fixed(c, 0xEB);
    Fixed(c, 0xFF);
    EmitPad(c, 28);          // the $FF out, then read mode
    c.MemX(Mn::LDA, 0xC08E); // Q7 off
    c.MemX(Mn::LDA, 0xC08C); // Q6 off
    c.Op(Mn::RTS);
}

} // namespace tisple
