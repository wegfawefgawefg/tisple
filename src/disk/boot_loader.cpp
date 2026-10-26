// the self-booting disk's loader. License: MIT as well as GPL-3.0-or-later (the runtime
// exception): it is copied onto users' disks. Original to Tisple; see boot_loader.hpp.
//
// $0800       2: the rom loads two sectors
// $0801-$09EF the code
// $09F0-$09FF the signature: "TISPLE BOOT", a version byte and the return pc
// while it runs it also uses:
// $0296-$02FF the nibble table it builds: $0200 + n is disk nibble n's 6-bit value
// $0300-$0355 the low bits of the sector being read
// $0356-$0365 which sectors of the track are in
//
// reading: the head is on track 0 when the rom jumps here. for each track the loader seeks
// (2.5 ms each way per half-track, 25 ms to settle), then reads whichever wanted sector comes
// under the head next, straight to its page, until the track's sectors are all in. putting a
// sector's low bits back takes about a sector's time, so a track takes two turns.
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
constexpr int kCodeEnd = 0x09F0;
constexpr int kSignature = 0x09F0;
constexpr int kTable = 0x0200; // + nibble: page-aligned, so the lookups never cross a page
constexpr int kAux = 0x0300;
constexpr int kDone = 0x0356;
constexpr uint8_t kLoaderVersion = 1;
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

struct LoaderPlan {
    uint16_t origin = 0;
    uint8_t first_page = 0;
    uint8_t pages = 0;
};

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
    c.MemY(Mn::STA, kAux);
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
    c.MemX(Mn::LSR, kAux);
    c.OpA(Mn::ROL);
    c.MemX(Mn::LSR, kAux);
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

// $0801: set up, then track by track
void EmitMain(Loader& l, const LoaderPlan& plan, std::int32_t seek, std::int32_t address,
              std::int32_t data, std::int32_t ret) {
    Code6502& c = l.c;
    std::int32_t track = c.Label("main.track");
    std::int32_t few = c.Label("main.few");
    std::int32_t clear = c.Label("main.clear");
    std::int32_t sector = c.Label("main.sector");
    std::int32_t elsewhere = c.Label("main.elsewhere");
    std::int32_t finish = c.Label("main.finish");
    c.Op(Mn::SEI);
    c.Op(Mn::CLD);
    c.Mem(Mn::STX, kSlot);
    c.MemX(Mn::LDA, 0xC089); // motor on, drive 1, read mode
    c.MemX(Mn::LDA, 0xC08A);
    c.MemX(Mn::LDA, 0xC08E);
    c.MemX(Mn::LDA, 0xC08C);
    c.Imm(Mn::LDA, 0);
    c.Mem(Mn::STA, kHead);
    c.Mem(Mn::STA, kPtr);
    c.Imm(Mn::LDA, kBootFirstTrack);
    c.Mem(Mn::STA, kTrack);
    c.Imm(Mn::LDA, plan.first_page);
    c.Mem(Mn::STA, kPage);
    c.Imm(Mn::LDA, plan.pages);
    c.Mem(Mn::STA, kLeft);
    c.Op(Mn::TXA); // the latch reads: $C08C + slot * 16
    c.Imm(Mn::ORA, 0x8C);
    for (std::int32_t site : l.latch_reads) {
        c.MemLabel(Mn::STA, site, 1);
    }
    EmitNibbleTable(l);

    c.Place(track);
    c.Mem(Mn::LDA, kTrack);
    c.OpA(Mn::ASL);
    c.Jump(Mn::JSR, seek);
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

    c.Place(sector);
    c.Jump(Mn::JSR, address);
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
    c.Mem(Mn::STA, kPtr + 1);
    c.Jump(Mn::JSR, data);
    c.Br(Mn::BCS, sector);
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
    c.Br(Mn::BEQ, finish);
    c.Mem(Mn::INC, kTrack);
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
    c.Mem(Mn::JSR, plan.origin);
    c.Place(ret);
    c.Mem(Mn::LDA, 0xC051); // the program came back: text, and the monitor
    c.Mem(Mn::JMP, 0xFF69);
}

// the loader's 512 bytes for this program, and where its call returns
bool BuildLoader(const LoaderPlan& plan, uint8_t out[2][kSectorBytes], uint16_t& return_pc,
                 std::string& error) {
    // the routines go after main in memory, but main patches their latch reads, so they
    // are written first and moved behind it
    Loader l;
    Code6502& c = l.c;
    std::int32_t seek = c.Label("seek");
    std::int32_t wait = c.Label("wait");
    std::int32_t nibble = c.Label("nibble");
    std::int32_t address = c.Label("address");
    std::int32_t data = c.Label("data");
    std::int32_t ret = c.Label("return");
    EmitSeek(l, seek, wait);
    EmitWait(l, wait);
    EmitNibble(l, nibble);
    EmitAddress(l, address, nibble);
    EmitData(l, data, nibble);
    std::vector<AsmItem> routines = c.as.items;
    c.as.items.clear();
    c.Byte(2);
    EmitMain(l, plan, seek, address, data, ret);
    c.as.items.insert(c.as.items.end(), routines.begin(), routines.end());
    std::vector<uint8_t> bytes;
    if (!BuildCode(c, kLoaderBase, bytes, error)) {
        return false;
    }
    if (kLoaderBase + bytes.size() > kCodeEnd) {
        error = "the boot loader is " + std::to_string(bytes.size()) + " bytes, more than fits";
        return false;
    }
    std::memset(out, 0, 2 * kSectorBytes);
    std::memcpy(out[0], bytes.data(), bytes.size() < 256 ? bytes.size() : 256);
    if (bytes.size() > 256) {
        std::memcpy(out[1], bytes.data() + 256, bytes.size() - 256);
    }
    return_pc = LabelAddress(c, ret);
    uint8_t* sig = out[1] + (kSignature - 0x0900);
    std::memcpy(sig, kSignatureText, sizeof(kSignatureText) - 1);
    sig[11] = kLoaderVersion;
    sig[12] = static_cast<uint8_t>(return_pc & 0xFF);
    sig[13] = static_cast<uint8_t>(return_pc >> 8);
    return true;
}

} // namespace

bool MakeBootDisk(const std::vector<uint8_t>& program, uint16_t origin, DiskSectors& disk,
                  BootDiskInfo& info, std::string& error) {
    if (program.empty()) {
        error = "the program is empty";
        return false;
    }
    uint32_t start = origin & 0xFF00u;
    uint32_t end = origin + static_cast<uint32_t>(program.size());
    uint32_t pages = (end - start + 0xFF) / 0x100;
    if (start < kBootLoadLow || end > kBootLoadHigh) {
        char text[160];
        std::snprintf(text, sizeof(text),
                      "a self-booting program must load in $%04X-$%04X; this one is $%04X-$%04X",
                      kBootLoadLow, kBootLoadHigh - 1, origin, end - 1);
        error = text;
        return false;
    }
    LoaderPlan plan;
    plan.origin = origin;
    plan.first_page = static_cast<uint8_t>(start >> 8);
    plan.pages = static_cast<uint8_t>(pages);
    uint8_t loader[2][kSectorBytes];
    uint16_t return_pc = 0;
    if (!BuildLoader(plan, loader, return_pc, error)) {
        return false;
    }
    FormatDos33(disk, kDefaultVolume);
    std::memcpy(disk.data[0][0], loader[0], kSectorBytes);
    std::memcpy(disk.data[0][1], loader[1], kSectorBytes);
    std::vector<uint8_t> image(pages * 0x100, 0);
    std::memcpy(image.data() + (origin - start), program.data(), program.size());
    for (uint32_t k = 0; k < pages; k++) {
        int track = kBootFirstTrack + static_cast<int>(k / kSectorsPerTrack);
        int physical = static_cast<int>(k % kSectorsPerTrack);
        std::memcpy(disk.data[track][physical], image.data() + k * 0x100, 0x100);
    }
    info.return_pc = return_pc;
    info.first_page = static_cast<uint16_t>(plan.first_page);
    info.pages = pages;
    info.last_track = kBootFirstTrack + static_cast<int>((pages - 1) / kSectorsPerTrack);
    for (int t = 0; t <= info.last_track; t++) {
        DosReserveTrack(disk, t);
    }
    return true;
}

std::string BootTracksText(const BootDiskInfo& info) {
    if (info.last_track == kBootFirstTrack) {
        return "track " + std::to_string(kBootFirstTrack);
    }
    return "tracks " + std::to_string(kBootFirstTrack) + "-" + std::to_string(info.last_track);
}

bool FindBootLoader(const DiskTrack& track0, uint16_t& return_pc) {
    uint8_t sectors[kSectorsPerTrack][kSectorBytes];
    bool found[kSectorsPerTrack];
    DenibblizeTrack(track0, 0, sectors, found);
    if (!found[1]) {
        return false;
    }
    const uint8_t* sig = sectors[1] + (kSignature - 0x0900);
    if (std::memcmp(sig, kSignatureText, sizeof(kSignatureText) - 1) != 0 ||
        sig[11] != kLoaderVersion) {
        return false;
    }
    return_pc = static_cast<uint16_t>(sig[12] | (sig[13] << 8));
    return true;
}

} // namespace tisple
