// the boot loader's disk reading: waiting, stepping the head, the next disk byte, address
// and data fields, and the nibble table. License: MIT as well as GPL-3.0-or-later, like the
// loader (boot_loader.hpp). The read loops never take more than 22 cycles a nibble.

#include "disk/loader_code.hpp"

namespace tisple {

// wait about A milliseconds (1000 cycles each); uses Y
void EmitWait(Loader& l, std::int32_t wait) {
    Code6502& c = l.c;
    std::int32_t outer = c.Label("wait.outer");
    std::int32_t inner = c.Label("wait.inner");
    c.Place(wait);
    c.Mem(Mn::STA, l.at.wait);
    c.Place(outer);
    c.Imm(Mn::LDY, 199);
    c.Place(inner);
    c.Op(Mn::DEY);
    c.Br(Mn::BNE, inner);
    c.Mem(Mn::DEC, l.at.wait);
    c.Br(Mn::BNE, outer);
    c.Op(Mn::RTS);
}

// move the head to the half-track in A: next phase on, wait, old phase off, wait
void EmitSeek(Loader& l, std::int32_t seek, std::int32_t wait) {
    Code6502& c = l.c;
    std::int32_t loop = c.Label("seek.loop");
    std::int32_t up = c.Label("seek.up");
    std::int32_t step = c.Label("seek.step");
    std::int32_t there = c.Label("seek.there");
    c.Place(seek);
    c.Mem(Mn::STA, l.at.target);
    c.Place(loop);
    c.Mem(Mn::LDA, l.at.head);
    c.Mem(Mn::CMP, l.at.target);
    c.Br(Mn::BEQ, there);
    c.Br(Mn::BCC, up);
    c.Imm(Mn::SBC, 1); // carry is set: head - 1
    c.Br(Mn::BCS, step);
    c.Place(up);
    c.Imm(Mn::ADC, 1); // carry is clear: head + 1
    c.Place(step);
    c.Mem(Mn::STA, l.at.next);
    l.PhaseIndex();
    c.MemX(Mn::LDA, 0xC081); // the next phase on
    c.Imm(Mn::LDA, 3);
    c.Jump(Mn::JSR, wait);
    c.Mem(Mn::LDA, l.at.head);
    l.PhaseIndex();
    c.MemX(Mn::LDA, 0xC080); // the old one off
    c.Mem(Mn::LDA, l.at.next);
    c.Mem(Mn::STA, l.at.head);
    c.Imm(Mn::LDA, 2);
    c.Jump(Mn::JSR, wait);
    c.Jump(Mn::JMP, loop);
    c.Place(there);
    c.Imm(Mn::LDA, 25); // let the head settle on its phase, then let go
    c.Jump(Mn::JSR, wait);
    c.Mem(Mn::LDA, l.at.head);
    l.PhaseIndex();
    c.MemX(Mn::LDA, 0xC080);
    c.Op(Mn::RTS);
}

// the next disk byte in X
void EmitNibble(Loader& l, std::int32_t nibble) {
    l.c.Place(nibble);
    l.ReadLatch(nibble);
    l.c.Op(Mn::RTS);
}

// the next good address field: track at kFields+2, sector at kFields+1
void EmitAddress(Loader& l, std::int32_t address, std::int32_t nibble) {
    Code6502& c = l.c;
    std::int32_t d5 = c.Label("addr.d5");
    std::int32_t odd = c.Label("addr.odd");
    std::int32_t even = c.Label("addr.even");
    c.Place(address);
    c.Jump(Mn::JSR, nibble);
    c.Place(d5);
    c.Imm(Mn::CPX, 0xD5);
    c.Br(Mn::BNE, address);
    c.Jump(Mn::JSR, nibble);
    c.Imm(Mn::CPX, 0xAA);
    c.Br(Mn::BNE, d5);
    c.Jump(Mn::JSR, nibble);
    c.Imm(Mn::CPX, 0x96);
    c.Br(Mn::BNE, d5);
    c.Imm(Mn::LDY, 3);
    c.Place(odd);
    l.ReadLatch(odd);
    c.Op(Mn::TXA);
    c.Op(Mn::SEC);
    c.OpA(Mn::ROL);
    c.Mem(Mn::STA, kTemp);
    c.Place(even);
    l.ReadLatch(even);
    c.Op(Mn::TXA);
    c.Mem(Mn::AND, kTemp);
    c.MemY(Mn::STA, kFields);
    c.Op(Mn::DEY);
    c.Br(Mn::BPL, odd);
    c.Mem(Mn::LDA, kFields);
    c.Mem(Mn::EOR, kFields + 1);
    c.Mem(Mn::EOR, kFields + 2);
    c.Mem(Mn::EOR, kFields + 3);
    c.Br(Mn::BNE, address);
    c.Op(Mn::RTS);
}

// the data field that follows, to (kPtr); carry set if it is not there or does not check
void EmitData(Loader& l, std::int32_t data, std::int32_t nibble) {
    Code6502& c = l.c;
    std::int32_t look = c.Label("data.look");
    std::int32_t d5 = c.Label("data.d5");
    std::int32_t aux = c.Label("data.aux");
    std::int32_t page = c.Label("data.page");
    std::int32_t sum = c.Label("data.sum");
    std::int32_t fix = c.Label("data.fix");
    std::int32_t fix_next = c.Label("data.fix_next");
    std::int32_t fail = c.Label("data.fail");
    c.Place(data);
    c.Imm(Mn::LDY, 32); // the mark is a dozen bytes on; give up after 32
    c.Place(look);
    c.Op(Mn::DEY);
    c.Br(Mn::BEQ, fail);
    c.Jump(Mn::JSR, nibble);
    c.Place(d5);
    c.Imm(Mn::CPX, 0xD5);
    c.Br(Mn::BNE, look);
    c.Jump(Mn::JSR, nibble);
    c.Imm(Mn::CPX, 0xAA);
    c.Br(Mn::BNE, d5);
    c.Jump(Mn::JSR, nibble);
    c.Imm(Mn::CPX, 0xAD);
    c.Br(Mn::BNE, fail);
    // each nibble is its value xor the one before: A carries the chain
    c.Imm(Mn::LDY, 0);
    c.Op(Mn::TYA);
    c.Place(aux);
    l.ReadLatch(aux);
    c.MemX(Mn::EOR, kTable);
    c.MemY(Mn::STA, kLowBits);
    c.Op(Mn::INY);
    c.Imm(Mn::CPY, 86);
    c.Br(Mn::BNE, aux);
    c.Imm(Mn::LDY, 0);
    c.Place(page);
    l.ReadLatch(page);
    c.MemX(Mn::EOR, kTable);
    c.IndY(Mn::STA, kPtr);
    c.Op(Mn::INY);
    c.Br(Mn::BNE, page);
    c.Place(sum);
    l.ReadLatch(sum);
    c.MemX(Mn::EOR, kTable);
    c.Br(Mn::BNE, fail);
    // byte i gets two low bits from entry i mod 86, the pair swapped
    c.Imm(Mn::LDX, 0);
    c.Place(fix);
    c.IndY(Mn::LDA, kPtr);
    c.MemX(Mn::LSR, kLowBits);
    c.OpA(Mn::ROL);
    c.MemX(Mn::LSR, kLowBits);
    c.OpA(Mn::ROL);
    c.IndY(Mn::STA, kPtr);
    c.Op(Mn::INX);
    c.Imm(Mn::CPX, 86);
    c.Br(Mn::BCC, fix_next);
    c.Imm(Mn::LDX, 0);
    c.Place(fix_next);
    c.Op(Mn::INY);
    c.Br(Mn::BNE, fix);
    c.Op(Mn::CLC);
    c.Op(Mn::RTS);
    c.Place(fail);
    c.Op(Mn::SEC);
    c.Op(Mn::RTS);
}

// the table of disk nibbles: a nibble has its top bit set, two neighbouring ones among bits
// 0-6 and at most one pair of neighbouring zeros; the 64 of them in order are values 0-63
void EmitNibbleTable(Loader& l) {
    Code6502& c = l.c;
    std::int32_t next = c.Label("table.next");
    std::int32_t skip = c.Label("table.skip");
    c.Imm(Mn::LDY, 0);
    c.Imm(Mn::LDX, 0x80);
    c.Place(next);
    c.Mem(Mn::STX, kTemp);
    c.Op(Mn::TXA);
    c.OpA(Mn::ASL); // carry = bit 7 = 1 until the SBC
    c.Mem(Mn::AND, kTemp);
    c.Imm(Mn::AND, 0x7E); // neighbouring ones
    c.Br(Mn::BEQ, skip);
    c.Op(Mn::TXA);
    c.OpA(Mn::ASL);
    c.Mem(Mn::ORA, kTemp);
    c.Imm(Mn::EOR, 0xFF);
    c.Imm(Mn::AND, 0x7E); // neighbouring zeros
    c.Mem(Mn::STA, kTemp);
    c.Imm(Mn::SBC, 1);
    c.Mem(Mn::AND, kTemp); // more than one such pair
    c.Br(Mn::BNE, skip);
    c.Op(Mn::TYA);
    c.MemX(Mn::STA, kTable);
    c.Op(Mn::INY);
    c.Place(skip);
    c.Op(Mn::INX);
    c.Br(Mn::BNE, next);
}

} // namespace tisple
