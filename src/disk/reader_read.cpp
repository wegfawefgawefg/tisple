// lib/disk's reader: address and data fields, putting a sector's bytes together where they go,
// and the tables. License: MIT as well as GPL-3.0-or-later (disk_reader.hpp). The read loops
// keep the boot loader's rule, never more than 22 cycles a nibble, with the latch reads
// patched for the slot. What is new: the values come out of the nibble tables already shifted
// into place (the high six bits times 4, the low entries with their pairs swapped), which xor
// keeps through the chain, so a sector is put together in one pass of about 4,300 cycles.

#include "disk/reader_code.hpp"

namespace tisple {

namespace {

// nibbles: about three turns, so a drive that is still coming up to speed has time
constexpr int kAddressBudget = 0x5000;

} // namespace

// the next good address field to the field bytes; carry clear with interrupts off (SEI from
// its first mark on), or carry set after kAddressBudget nibbles with none, the I flag the
// caller's. Interrupts stay as the caller has them while it looks
void EmitReaderAddress(Reader& r, std::int32_t address) {
    Code6502& c = r.l.c;
    std::int32_t retry = c.Label("raddr.retry");
    std::int32_t look = c.Label("raddr.look");
    std::int32_t check = c.Label("raddr.check");
    std::int32_t aa = c.Label("raddr.aa");
    std::int32_t mark = c.Label("raddr.96");
    std::int32_t odd = c.Label("raddr.odd");
    std::int32_t even = c.Label("raddr.even");
    std::int32_t timeout = c.Label("raddr.timeout");
    c.Place(address);
    c.Imm(Mn::LDA, kAddressBudget & 0xFF);
    c.Mem(Mn::STA, r.Var(kRvCountLo));
    c.Imm(Mn::LDA, kAddressBudget >> 8);
    c.Mem(Mn::STA, r.Var(kRvCountHi));
    c.Br(Mn::BNE, look);
    c.Place(retry);
    r.RestoreI();
    c.Place(look);
    r.l.ReadLatch(look);
    c.Mem(Mn::DEC, r.Var(kRvCountLo));
    c.Br(Mn::BNE, check);
    c.Mem(Mn::DEC, r.Var(kRvCountHi));
    c.Br(Mn::BEQ, timeout);
    c.Place(check);
    c.Imm(Mn::CPX, 0xD5);
    c.Br(Mn::BNE, look);
    c.Op(Mn::SEI); // a tick from here would cost the field
    c.Place(aa);
    r.l.ReadLatch(aa);
    c.Imm(Mn::CPX, 0xAA);
    c.Br(Mn::BNE, retry);
    c.Place(mark);
    r.l.ReadLatch(mark);
    c.Imm(Mn::CPX, 0x96);
    c.Br(Mn::BNE, retry);
    c.Imm(Mn::LDY, 3);
    c.Place(odd);
    r.l.ReadLatch(odd);
    c.Op(Mn::TXA);
    c.Op(Mn::SEC);
    c.OpA(Mn::ROL);
    c.Mem(Mn::STA, r.Var(kRvTemp));
    c.Place(even);
    r.l.ReadLatch(even);
    c.Op(Mn::TXA);
    c.Mem(Mn::AND, r.Var(kRvTemp));
    c.MemY(Mn::STA, r.Var(kRvField));
    c.Op(Mn::DEY);
    c.Br(Mn::BPL, odd);
    c.Mem(Mn::LDA, r.Var(kRvField));
    c.Mem(Mn::EOR, r.Var(kRvField + 1));
    c.Mem(Mn::EOR, r.Var(kRvField + 2));
    c.Mem(Mn::EOR, r.Var(kRvField + 3));
    c.Br(Mn::BNE, retry);
    c.Op(Mn::CLC);
    c.Op(Mn::RTS);
    c.Place(timeout);
    c.Op(Mn::SEC);
    c.Op(Mn::RTS);
}

// the data field that follows: the 86 low entries to the entries, the 256 high values (times
// 4) to the bounce page; carry set if it is not there or does not check. Interrupts are off
void EmitReaderData(Reader& r, std::int32_t data) {
    Code6502& c = r.l.c;
    std::int32_t look = c.Label("rdata.look");
    std::int32_t first = c.Label("rdata.first");
    std::int32_t d5 = c.Label("rdata.d5");
    std::int32_t aa = c.Label("rdata.aa");
    std::int32_t ad = c.Label("rdata.ad");
    std::int32_t low = c.Label("rdata.low");
    std::int32_t last = c.Label("rdata.last");
    std::int32_t high = c.Label("rdata.high");
    std::int32_t sum = c.Label("rdata.sum");
    std::int32_t fail = c.Label("rdata.fail");
    c.Place(data);
    c.Imm(Mn::LDY, 32); // the mark is a dozen bytes on; give up after 32
    c.Place(look);
    c.Op(Mn::DEY);
    c.Br(Mn::BEQ, fail);
    c.Place(first);
    r.l.ReadLatch(first);
    c.Place(d5);
    c.Imm(Mn::CPX, 0xD5);
    c.Br(Mn::BNE, look);
    c.Place(aa);
    r.l.ReadLatch(aa);
    c.Imm(Mn::CPX, 0xAA);
    c.Br(Mn::BNE, d5);
    c.Place(ad);
    r.l.ReadLatch(ad);
    c.Imm(Mn::CPX, 0xAD);
    c.Br(Mn::BNE, fail);
    // each nibble is its value xor the one before: A carries the chain
    c.Imm(Mn::LDY, 0);
    c.Op(Mn::TYA);
    c.Place(low);
    r.l.ReadLatch(low);
    c.MemX(Mn::EOR, r.LowTable());
    c.MemY(Mn::STA, r.Entries());
    c.Op(Mn::INY);
    c.Imm(Mn::CPY, 85);
    c.Br(Mn::BNE, low);
    // the last entry, and the chain from swapped pairs to times 4: 22 cycles to the next read
    c.Place(last);
    r.l.ReadLatch(last);
    c.MemX(Mn::EOR, r.LowTable());
    c.Mem(Mn::STA, r.Entries() + 85);
    c.Op(Mn::TAX);
    c.MemX(Mn::LDA, r.U());
    c.Imm(Mn::LDY, 0);
    c.Place(high);
    r.l.ReadLatch(high);
    c.MemX(Mn::EOR, r.HighTable());
    c.MemY(Mn::STA, r.bounce);
    c.Op(Mn::INY);
    c.Br(Mn::BNE, high);
    c.Place(sum);
    r.l.ReadLatch(sum);
    c.MemX(Mn::EOR, r.HighTable());
    c.Br(Mn::BNE, fail);
    c.Op(Mn::CLC);
    c.Op(Mn::RTS);
    c.Place(fail);
    c.Op(Mn::SEC);
    c.Op(Mn::RTS);
}

// byte i is the bounce page's i or'ed with entry i mod 86's pair for its third: entries
// 0-83 give three bytes, 84 and 85 two. The stores' addresses are patched (s0, s86, s172 for
// the first loop, t0 and t86 for the second): 50 cycles an entry
void EmitReaderFix(Reader& r, std::int32_t fix, std::int32_t s0, std::int32_t s86,
                   std::int32_t s172, std::int32_t t0, std::int32_t t86) {
    Code6502& c = r.l.c;
    std::int32_t three = c.Label("rfix.three");
    std::int32_t two = c.Label("rfix.two");
    c.Place(fix);
    c.Imm(Mn::LDY, 0);
    c.Place(three);
    c.MemY(Mn::LDX, r.Entries());
    c.Op(Mn::TXA);
    c.Imm(Mn::AND, 3);
    c.MemY(Mn::ORA, r.bounce);
    c.Place(s0);
    c.MemY(Mn::STA, r.bounce);
    c.MemY(Mn::LDA, r.bounce + 86);
    c.MemX(Mn::ORA, r.T2());
    c.Place(s86);
    c.MemY(Mn::STA, r.bounce + 86);
    c.MemY(Mn::LDA, r.bounce + 172);
    c.MemX(Mn::ORA, r.T3());
    c.Place(s172);
    c.MemY(Mn::STA, r.bounce + 172);
    c.Op(Mn::INY);
    c.Imm(Mn::CPY, 84);
    c.Br(Mn::BNE, three);
    c.Place(two);
    c.MemY(Mn::LDX, r.Entries());
    c.Op(Mn::TXA);
    c.Imm(Mn::AND, 3);
    c.MemY(Mn::ORA, r.bounce);
    c.Place(t0);
    c.MemY(Mn::STA, r.bounce);
    c.MemY(Mn::LDA, r.bounce + 86);
    c.MemX(Mn::ORA, r.T2());
    c.Place(t86);
    c.MemY(Mn::STA, r.bounce + 86);
    c.Op(Mn::INY);
    c.Imm(Mn::CPY, 86);
    c.Br(Mn::BNE, two);
    c.Op(Mn::RTS);
}

// T2, T3 (an entry's pair for the second and third thirds; the first is AND #3), U (swapped pairs
// to the value times 4), then the two nibble tables: the loader's walk over the 64 data nibbles in
// order
void EmitReaderTables(Reader& r, std::int32_t tables) {
    Code6502& c = r.l.c;
    std::int32_t small = c.Label("rtab.small");
    std::int32_t next = c.Label("rtab.next");
    std::int32_t skip = c.Label("rtab.skip");
    int temp = r.Var(kRvTemp);
    c.Place(tables);
    c.Imm(Mn::LDY, 63);
    c.Place(small);
    c.Op(Mn::TYA);
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.Imm(Mn::AND, 3);
    c.MemY(Mn::STA, r.T2());
    c.Op(Mn::TYA);
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.MemY(Mn::STA, r.T3());
    c.Op(Mn::TYA); // its pairs swapped, times 4
    c.Imm(Mn::AND, 0x15);
    c.OpA(Mn::ASL);
    c.Mem(Mn::STA, temp);
    c.Op(Mn::TYA);
    c.OpA(Mn::LSR);
    c.Imm(Mn::AND, 0x15);
    c.Mem(Mn::ORA, temp);
    c.OpA(Mn::ASL);
    c.OpA(Mn::ASL);
    c.MemY(Mn::STA, r.U());
    c.Op(Mn::DEY);
    c.Br(Mn::BPL, small);
    // a nibble has its top bit set, two neighbouring ones among bits 0-6 and at most one pair
    // of neighbouring zeros; the 64 of them in order are values 0-63
    c.Imm(Mn::LDY, 0);
    c.Imm(Mn::LDX, 0x80);
    c.Place(next);
    c.Mem(Mn::STX, temp);
    c.Op(Mn::TXA);
    c.OpA(Mn::ASL); // carry = bit 7 = 1 until the SBC
    c.Mem(Mn::AND, temp);
    c.Imm(Mn::AND, 0x7E); // neighbouring ones
    c.Br(Mn::BEQ, skip);
    c.Op(Mn::TXA);
    c.OpA(Mn::ASL);
    c.Mem(Mn::ORA, temp);
    c.Imm(Mn::EOR, 0xFF);
    c.Imm(Mn::AND, 0x7E); // neighbouring zeros
    c.Mem(Mn::STA, temp);
    c.Imm(Mn::SBC, 1);
    c.Mem(Mn::AND, temp); // more than one such pair
    c.Br(Mn::BNE, skip);
    c.Op(Mn::TYA);
    c.OpA(Mn::ASL);
    c.OpA(Mn::ASL);
    c.MemX(Mn::STA, r.HighTable());
    c.MemY(Mn::LDA, r.U()); // the value with its pairs swapped: U's over 4
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.MemX(Mn::STA, r.LowTable());
    c.Op(Mn::INY);
    c.Place(skip);
    c.Op(Mn::INX);
    c.Br(Mn::BNE, next);
    c.Op(Mn::RTS);
}

} // namespace tisple
