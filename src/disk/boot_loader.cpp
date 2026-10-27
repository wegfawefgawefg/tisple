// the self-booting disk's loader. License: MIT as well as GPL-3.0-or-later (the runtime
// exception): it is copied onto users' disks. Original to Tisple; see boot_loader.hpp.
//
// $0800       3: the rom loads three sectors
// $0801-$0AEF the code, then the segment table
// $0AF0-$0AFF the signature: "TISPLE BOOT", a version byte and the return pc (version 1, two
//             sectors long, had it at $09F0)
// while it runs it also uses:
// $0296-$02FF the nibble table it builds: $0200 + n is disk nibble n's 6-bit value
// $0300-$0355 the low bits of the sector being read
// $0356-$0365 which sectors of the track are in
// $0400-$04FF where an aux sector is read before it is copied over (text page 1)
//
// reading: the head is on track 0 when the rom jumps here. segment by segment (main memory,
// the language card's bank 2, or aux memory), each from a track of its own, the loader seeks
// (2.5 ms each way per half-track, 25 ms to settle), then reads whichever wanted sector comes
// under the head next, straight to its page, until the track's sectors are all in. putting a
// sector's low bits back takes about a sector's time, so a track takes two turns. track 17
// is skipped: the DOS 3.3 catalog beside the program lives there.
// the read loops never take more than 22 cycles a nibble; the latch reads are patched for
// the slot at the start, which leaves X and Y free.

#include "disk/boot_loader.hpp"

#include "disk/code6502.hpp"
#include "disk/dos33.hpp"

#include <cstdio>
#include <cstring>

namespace tisple {

namespace {

constexpr int kLoaderBase = 0x0800;
constexpr int kCodeEnd = 0x0AF0;
constexpr int kSignature = 0x0AF0;
constexpr int kTable = 0x0200; // + nibble: page-aligned, so the lookups never cross a page
constexpr int kLowBits = 0x0300;
constexpr int kDone = 0x0356;
constexpr int kBounce = 0x0400; // an aux sector, before its copy
constexpr uint8_t kLoaderVersion = 2;
const char kSignatureText[] = "TISPLE BOOT";

// zero page
constexpr int kSlot = 0xE0;
constexpr int kHead = 0xE1;   // the head's half-track
constexpr int kTarget = 0xE2; // seek target
constexpr int kNext = 0xE3;   // the half-track being stepped to
constexpr int kFields = 0xE4; // address field, last first: checksum, sector, track, volume
constexpr int kTrack = 0xE8;
constexpr int kPage = 0xE9;  // the page that sector 0 of this track goes to
constexpr int kLeft = 0xEA;  // pages still to read
constexpr int kCount = 0xEB; // sectors wanted on this track
constexpr int kNeed = 0xEC;  // of those, not read yet
constexpr int kTemp = 0xED;
constexpr int kPtr = 0xEE; // and $EF: where the sector goes
constexpr int kWait = 0xDF;
constexpr int kSegment = 0xDE; // the next segment's offset in the table
constexpr int kMode = 0xDD;    // this segment's bank: 0 main, 1 language card, 2 aux
constexpr int kDest = 0xDB;    // and $DC: an aux sector's page

struct Loader {
    Code6502 c;
    std::vector<std::int32_t> latch_reads; // LDX $C08C+slot instructions to patch

    // LDX latch: the address is patched at boot
    void ReadLatch(std::int32_t loop) {
        std::int32_t site = c.Label("latch");
        c.Place(site);
        c.Mem(Mn::LDX, 0xC08C);
        c.Br(Mn::BPL, loop);
        latch_reads.push_back(site);
    }
    // X = slot * 16 + phase * 2 for the half-track in A
    void PhaseIndex() {
        c.Imm(Mn::AND, 3);
        c.OpA(Mn::ASL);
        c.Mem(Mn::ORA, kSlot);
        c.Op(Mn::TAX);
    }
};

// wait about A milliseconds (1000 cycles each); uses Y
void EmitWait(Loader& l, std::int32_t wait) {
    Code6502& c = l.c;
    std::int32_t outer = c.Label("wait.outer");
    std::int32_t inner = c.Label("wait.inner");
    c.Place(wait);
    c.Mem(Mn::STA, kWait);
    c.Place(outer);
    c.Imm(Mn::LDY, 199);
    c.Place(inner);
    c.Op(Mn::DEY);
    c.Br(Mn::BNE, inner);
    c.Mem(Mn::DEC, kWait);
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
    c.Mem(Mn::STA, kTarget);
    c.Place(loop);
    c.Mem(Mn::LDA, kHead);
    c.Mem(Mn::CMP, kTarget);
    c.Br(Mn::BEQ, there);
    c.Br(Mn::BCC, up);
    c.Imm(Mn::SBC, 1); // carry is set: head - 1
    c.Br(Mn::BCS, step);
    c.Place(up);
    c.Imm(Mn::ADC, 1); // carry is clear: head + 1
    c.Place(step);
    c.Mem(Mn::STA, kNext);
    l.PhaseIndex();
    c.MemX(Mn::LDA, 0xC081); // the next phase on
    c.Imm(Mn::LDA, 3);
    c.Jump(Mn::JSR, wait);
    c.Mem(Mn::LDA, kHead);
    l.PhaseIndex();
    c.MemX(Mn::LDA, 0xC080); // the old one off
    c.Mem(Mn::LDA, kNext);
    c.Mem(Mn::STA, kHead);
    c.Imm(Mn::LDA, 2);
    c.Jump(Mn::JSR, wait);
    c.Jump(Mn::JMP, loop);
    c.Place(there);
    c.Imm(Mn::LDA, 25); // let the head settle on its phase, then let go
    c.Jump(Mn::JSR, wait);
    c.Mem(Mn::LDA, kHead);
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

// the next track, past the catalog's
void EmitNextTrack(Loader& l, std::int32_t next_track) {
    Code6502& c = l.c;
    std::int32_t done = c.Label("next.done");
    c.Place(next_track);
    c.Mem(Mn::INC, kTrack);
    c.Mem(Mn::LDA, kTrack);
    c.Imm(Mn::CMP, kBootCatalogTrack);
    c.Br(Mn::BNE, done);
    c.Mem(Mn::INC, kTrack);
    c.Place(done);
    c.Op(Mn::RTS);
}

// the sector at kBounce to the aux page at kDest: RAMWRT on, copy, off
void EmitToAux(Loader& l, std::int32_t to_aux) {
    Code6502& c = l.c;
    std::int32_t copy = c.Label("aux.copy");
    c.Place(to_aux);
    c.Imm(Mn::LDY, 0);
    c.Mem(Mn::STY, kDest);
    c.Mem(Mn::STA, 0xC005);
    c.Place(copy);
    c.MemY(Mn::LDA, kBounce);
    c.IndY(Mn::STA, kDest);
    c.Op(Mn::INY);
    c.Br(Mn::BNE, copy);
    c.Mem(Mn::STA, 0xC004);
    c.Op(Mn::RTS);
}

struct LoaderLabels {
    std::int32_t seek = -1;
    std::int32_t address = -1;
    std::int32_t data = -1;
    std::int32_t next_track = -1;
    std::int32_t to_aux = -1;
    std::int32_t table = -1;
    std::int32_t ret = -1;
};

// each segment from the table: first page, pages and bank; 0 pages ends the table. the
// language card's bank 2 is switched in for writes (two reads of $C083)
void EmitSegmentStart(Loader& l, const LoaderLabels& at, std::int32_t segment, std::int32_t track,
                      std::int32_t finish) {
    Code6502& c = l.c;
    std::int32_t go = c.Label("segment.go");
    c.Place(segment);
    c.Mem(Mn::LDX, kSegment);
    c.MemLabelX(Mn::LDA, at.table, 1);
    c.Br(Mn::BNE, go);
    c.Jump(Mn::JMP, finish);
    c.Place(go);
    c.Mem(Mn::STA, kLeft);
    c.MemLabelX(Mn::LDA, at.table, 0);
    c.Mem(Mn::STA, kPage);
    c.MemLabelX(Mn::LDA, at.table, 2);
    c.Mem(Mn::STA, kMode);
    c.Op(Mn::INX);
    c.Op(Mn::INX);
    c.Op(Mn::INX);
    c.Mem(Mn::STX, kSegment);
    c.Imm(Mn::CMP, static_cast<int>(LoaderBank::kCard));
    c.Br(Mn::BNE, track);
    c.Mem(Mn::LDA, 0xC083);
    c.Mem(Mn::LDA, 0xC083);
}

// $0801: set up, then segment by segment, track by track
void EmitMain(Loader& l, const LoaderPlan& plan, const LoaderLabels& at) {
    Code6502& c = l.c;
    std::int32_t segment = c.Label("main.segment");
    std::int32_t track = c.Label("main.track");
    std::int32_t few = c.Label("main.few");
    std::int32_t clear = c.Label("main.clear");
    std::int32_t sector = c.Label("main.sector");
    std::int32_t direct = c.Label("main.direct");
    std::int32_t counted = c.Label("main.counted");
    std::int32_t elsewhere = c.Label("main.elsewhere");
    std::int32_t more = c.Label("main.more");
    std::int32_t finish = c.Label("main.finish");
    c.Op(Mn::SEI);
    c.Op(Mn::CLD);
    c.Mem(Mn::STA, 0xC000); // 80store off: text page 1 is main memory
    c.Mem(Mn::STX, kSlot);
    c.MemX(Mn::LDA, 0xC089); // motor on, drive 1, read mode
    c.MemX(Mn::LDA, 0xC08A);
    c.MemX(Mn::LDA, 0xC08E);
    c.MemX(Mn::LDA, 0xC08C);
    c.Imm(Mn::LDA, 0);
    c.Mem(Mn::STA, kHead);
    c.Mem(Mn::STA, kPtr);
    c.Mem(Mn::STA, kSegment);
    c.Imm(Mn::LDA, kBootFirstTrack);
    c.Mem(Mn::STA, kTrack);
    c.Op(Mn::TXA); // the latch reads: $C08C + slot * 16
    c.Imm(Mn::ORA, 0x8C);
    for (std::int32_t site : l.latch_reads) {
        c.MemLabel(Mn::STA, site, 1);
    }
    EmitNibbleTable(l);
    EmitSegmentStart(l, at, segment, track, finish);

    c.Place(track);
    c.Mem(Mn::LDA, kTrack);
    c.OpA(Mn::ASL);
    c.Jump(Mn::JSR, at.seek);
    c.Mem(Mn::LDA, kLeft);
    c.Imm(Mn::CMP, 16);
    c.Br(Mn::BCC, few);
    c.Imm(Mn::LDA, 16);
    c.Place(few);
    c.Mem(Mn::STA, kCount);
    c.Mem(Mn::STA, kNeed);
    c.Imm(Mn::LDX, 15);
    c.Imm(Mn::LDA, 0);
    c.Place(clear);
    c.MemX(Mn::STA, kDone);
    c.Op(Mn::DEX);
    c.Br(Mn::BPL, clear);

    // a wanted sector to its page; an aux one by way of kBounce
    c.Place(sector);
    c.Jump(Mn::JSR, at.address);
    c.Mem(Mn::LDA, kFields + 2);
    c.Mem(Mn::CMP, kTrack);
    c.Br(Mn::BNE, elsewhere);
    c.Mem(Mn::LDX, kFields + 1);
    c.Mem(Mn::CPX, kCount);
    c.Br(Mn::BCS, sector);
    c.MemX(Mn::LDA, kDone);
    c.Br(Mn::BNE, sector);
    c.Op(Mn::TXA);
    c.Op(Mn::CLC);
    c.Mem(Mn::ADC, kPage);
    c.Mem(Mn::STA, kDest + 1);
    c.Mem(Mn::LDY, kMode);
    c.Imm(Mn::CPY, static_cast<int>(LoaderBank::kAux));
    c.Br(Mn::BNE, direct);
    c.Imm(Mn::LDA, kBounce >> 8);
    c.Place(direct);
    c.Mem(Mn::STA, kPtr + 1);
    c.Jump(Mn::JSR, at.data);
    c.Br(Mn::BCS, sector);
    c.Mem(Mn::LDA, kMode);
    c.Imm(Mn::CMP, static_cast<int>(LoaderBank::kAux));
    c.Br(Mn::BNE, counted);
    c.Jump(Mn::JSR, at.to_aux);
    c.Place(counted);
    c.Mem(Mn::LDX, kFields + 1);
    c.MemX(Mn::INC, kDone);
    c.Mem(Mn::DEC, kNeed);
    c.Br(Mn::BNE, sector);
    c.Mem(Mn::LDA, kPage); // the track is in
    c.Op(Mn::CLC);
    c.Mem(Mn::ADC, kCount);
    c.Mem(Mn::STA, kPage);
    c.Mem(Mn::LDA, kLeft);
    c.Op(Mn::SEC);
    c.Mem(Mn::SBC, kCount);
    c.Mem(Mn::STA, kLeft);
    c.Jump(Mn::JSR, at.next_track); // every segment starts on a track of its own
    c.Mem(Mn::LDA, kLeft);
    c.Br(Mn::BNE, more);
    c.Jump(Mn::JMP, segment);
    c.Place(more);
    c.Jump(Mn::JMP, track);
    c.Place(elsewhere); // the head is on another track: start from there
    c.OpA(Mn::ASL);
    c.Mem(Mn::STA, kHead);
    c.Jump(Mn::JMP, track);

    c.Place(finish);
    c.Mem(Mn::LDX, kSlot);
    c.MemX(Mn::LDA, 0xC088); // motor off
    c.Mem(Mn::LDA, 0xC082);  // language card: rom reads, no writes
    c.Imm(Mn::LDX, 0xFF);
    c.Op(Mn::TXS);
    c.Imm(Mn::LDA, 0);
    c.Op(Mn::TAX);
    c.Op(Mn::TAY);
    c.Mem(Mn::JSR, plan.entry);
    c.Place(at.ret);
    c.Mem(Mn::LDA, 0xC051); // the program came back: text, and the monitor
    c.Mem(Mn::JMP, 0xFF69);
}

} // namespace

bool BuildLoader(const LoaderPlan& plan, uint8_t out[kBootLoaderSectors][kSectorBytes],
                 uint16_t& return_pc, std::string& error) {
    if (plan.segments.empty() || plan.segments.size() > kMaxBootSegments) {
        error = "a boot disk holds 1 to " + std::to_string(kMaxBootSegments) + " segments, not " +
                std::to_string(plan.segments.size());
        return false;
    }
    // the routines go after main in memory, but main patches their latch reads, so they
    // are written first and moved behind it
    Loader l;
    Code6502& c = l.c;
    LoaderLabels at;
    at.seek = c.Label("seek");
    std::int32_t wait = c.Label("wait");
    std::int32_t nibble = c.Label("nibble");
    at.address = c.Label("address");
    at.data = c.Label("data");
    at.next_track = c.Label("next_track");
    at.to_aux = c.Label("to_aux");
    at.table = c.Label("table");
    at.ret = c.Label("return");
    EmitSeek(l, at.seek, wait);
    EmitWait(l, wait);
    EmitNibble(l, nibble);
    EmitAddress(l, at.address, nibble);
    EmitData(l, at.data, nibble);
    EmitNextTrack(l, at.next_track);
    EmitToAux(l, at.to_aux);
    c.Place(at.table);
    for (const LoaderSegment& segment : plan.segments) {
        c.Byte(segment.first_page);
        c.Byte(segment.pages);
        c.Byte(static_cast<int>(segment.bank));
    }
    c.Byte(0);
    c.Byte(0); // pages 0: the end
    std::vector<AsmItem> routines = c.as.items;
    c.as.items.clear();
    c.Byte(kBootLoaderSectors);
    EmitMain(l, plan, at);
    c.as.items.insert(c.as.items.end(), routines.begin(), routines.end());
    std::vector<uint8_t> bytes;
    if (!BuildCode(c, kLoaderBase, bytes, error)) {
        return false;
    }
    if (kLoaderBase + bytes.size() > kCodeEnd) {
        error = "the boot loader is " + std::to_string(bytes.size()) + " bytes, more than fits";
        return false;
    }
    std::memset(out, 0, kBootLoaderSectors * kSectorBytes);
    for (size_t i = 0; i < bytes.size(); i++) {
        out[i / kSectorBytes][i % kSectorBytes] = bytes[i];
    }
    return_pc = LabelAddress(c, at.ret);
    uint8_t* sig = out[2] + (kSignature - 0x0A00);
    std::memcpy(sig, kSignatureText, sizeof(kSignatureText) - 1);
    sig[11] = kLoaderVersion;
    sig[12] = static_cast<uint8_t>(return_pc & 0xFF);
    sig[13] = static_cast<uint8_t>(return_pc >> 8);
    return true;
}

namespace {

// the signature at the end of a loader sector, if it is there with this version
bool ReadSignature(const uint8_t* sector, bool found, uint8_t version, uint16_t& return_pc) {
    const uint8_t* sig = sector + 0xF0;
    if (!found || std::memcmp(sig, kSignatureText, sizeof(kSignatureText) - 1) != 0 ||
        sig[11] != version) {
        return false;
    }
    return_pc = static_cast<uint16_t>(sig[12] | (sig[13] << 8));
    return true;
}

} // namespace

bool FindBootLoader(const DiskTrack& track0, uint16_t& return_pc) {
    uint8_t sectors[kSectorsPerTrack][kSectorBytes];
    bool found[kSectorsPerTrack];
    DenibblizeTrack(track0, 0, sectors, found);
    // version 1 was two sectors and loaded one stretch of main memory
    return ReadSignature(sectors[2], found[2], kLoaderVersion, return_pc) ||
           ReadSignature(sectors[1], found[1], 1, return_pc);
}

} // namespace tisple
