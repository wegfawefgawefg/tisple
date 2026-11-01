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
// $0400-$07FF text page 1, cleared to spaces with LOADING, the percent and the meter in the
//             middle (not its holes)
// the bounce page, the first page of a main segment that loads after the aux ones: where an
//             aux sector is read before it is copied over
//
// reading: the head is on track 0 when the rom jumps here. segment by segment (main memory,
// the language card's bank 2, or aux memory), each from a track of its own, the loader seeks
// (2.5 ms each way per half-track, 25 ms to settle), then reads whichever wanted sector comes
// under the head next, straight to its page, until the track's sectors are all in. putting a
// sector's low bits back takes about a sector's time, so a track takes two turns. track 17
// is skipped: the DOS 3.3 catalog beside the program lives there. once a track is in, the
// meter counts its pages, before the seek.
// the read loops never take more than 22 cycles a nibble; the latch reads are patched for
// the slot at the start, which leaves X and Y free.

#include "disk/boot_loader.hpp"

#include "disk/dos33.hpp"
#include "disk/loader_code.hpp"

#include <cstdio>
#include <cstring>

namespace tisple {

namespace {

// the next track, past the catalog's (in line: it is the one place, and the loader's room is
// short)
void EmitNextTrack(Loader& l) {
    Code6502& c = l.c;
    std::int32_t done = c.Label("next.done");
    c.Mem(Mn::INC, kTrack);
    c.Mem(Mn::LDA, kTrack);
    c.Imm(Mn::CMP, kBootCatalogTrack);
    c.Br(Mn::BNE, done);
    c.Mem(Mn::INC, kTrack);
    c.Place(done);
}

// the sector at the bounce page to the aux page at kDest: RAMWRT on, copy, off
void EmitToAux(Loader& l, std::int32_t to_aux) {
    Code6502& c = l.c;
    std::int32_t copy = c.Label("aux.copy");
    c.Place(to_aux);
    c.Imm(Mn::LDY, 0);
    c.Mem(Mn::STY, kDest);
    c.Mem(Mn::STA, 0xC005);
    c.Place(copy);
    c.MemY(Mn::LDA, l.bounce);
    c.IndY(Mn::STA, kDest);
    c.Op(Mn::INY);
    c.Br(Mn::BNE, copy);
    c.Mem(Mn::STA, 0xC004);
    c.Op(Mn::RTS);
}

// LOADING and the percent, 0% to start, in row 12 from column 14; under it, in row 14 from
// column 10, the meter's 20 cells (5% each), empty to start
constexpr char kLoading[] = "LOADING   0%";
constexpr int kLoadingAt = 0x0628 + 14;
constexpr int kPercentAt = kLoadingAt + 8; // hundreds, tens, ones
constexpr int kMeterCells = 20;
constexpr int kMeterAt = 0x0728 + 10;
constexpr int kEmptyCell = 0xDF; // _
constexpr int kFullCell = 0x20;  // an inverse space

// text page 1 shown, 40 columns, cleared to spaces but for LOADING and the empty meter; the
// screen holes (the last 8 bytes of each 128) are the cards' and stay as they are
void EmitScreen(Loader& l, std::int32_t message) {
    Code6502& c = l.c;
    std::int32_t clear = c.Label("screen.clear");
    std::int32_t show = c.Label("screen.show");
    std::int32_t empty = c.Label("screen.empty");
    c.Mem(Mn::STA, 0xC00C); // 80 columns off
    c.Mem(Mn::LDA, 0xC051); // text
    c.Mem(Mn::LDA, 0xC054); // page 1
    c.Imm(Mn::LDA, 0xA0);
    c.Imm(Mn::LDY, 119);
    c.Place(clear);
    for (int block = 0; block < 8; block++) {
        c.MemY(Mn::STA, 0x0400 + block * 0x80);
    }
    c.Op(Mn::DEY);
    c.Br(Mn::BPL, clear);
    c.Imm(Mn::LDX, static_cast<int>(sizeof(kLoading)) - 2);
    c.Place(show);
    c.MemLabelX(Mn::LDA, message, 0);
    c.MemX(Mn::STA, kLoadingAt);
    c.Op(Mn::DEX);
    c.Br(Mn::BPL, show);
    c.Imm(Mn::LDX, kMeterCells - 1);
    c.Imm(Mn::LDA, kEmptyCell);
    c.Place(empty);
    c.MemX(Mn::STA, kMeterAt);
    c.Op(Mn::DEX);
    c.Br(Mn::BPL, empty);
}

// the meter once a track is in (in line, like the next track): for each of its kCount pages,
// 100 more in kMeter, and a percent more for each total pages there. The percent counts up on
// the screen itself, from the ones digit (a space counts as 0), and each 5% fills the next cell
// (the STA's address moves on). About 35 cycles a page and 60 a percent, before the seek, so no
// sector waits for it
void EmitProgress(Loader& l, uint32_t total) {
    Code6502& c = l.c;
    std::int32_t page = c.Label("progress.page");
    std::int32_t check = c.Label("progress.check");
    std::int32_t digit = c.Label("progress.digit");
    std::int32_t up = c.Label("progress.up");
    std::int32_t cell = c.Label("progress.cell");
    std::int32_t fill = c.Label("progress.fill");
    std::int32_t next = c.Label("progress.next");
    c.Mem(Mn::LDY, kCount);
    c.Place(page);
    c.Mem(Mn::LDA, kMeter);
    c.Op(Mn::CLC);
    c.Imm(Mn::ADC, 100);
    c.Mem(Mn::STA, kMeter);
    c.Br(Mn::BCC, check);
    c.Mem(Mn::INC, kMeter + 1);
    c.Place(check);
    c.Mem(Mn::LDA, kMeter);
    c.Op(Mn::SEC);
    c.Imm(Mn::SBC, static_cast<int>(total & 0xFF));
    c.Op(Mn::TAX);
    c.Mem(Mn::LDA, kMeter + 1);
    c.Imm(Mn::SBC, static_cast<int>(total >> 8));
    c.Br(Mn::BCC, next);
    c.Mem(Mn::STA, kMeter + 1);
    c.Mem(Mn::STX, kMeter);
    c.Imm(Mn::LDX, 2);
    c.Place(digit);
    c.MemX(Mn::LDA, kPercentAt);
    c.Imm(Mn::ORA, 0x30); // a space ($A0) reads as 0 ($B0)
    c.Imm(Mn::CMP, '9' | 0x80);
    c.Br(Mn::BCC, up);
    c.Imm(Mn::LDA, '0' | 0x80); // 9: 0, and the next digit up
    c.MemX(Mn::STA, kPercentAt);
    c.Op(Mn::DEX);
    c.Br(Mn::BPL, digit);
    c.Place(up);
    c.Imm(Mn::ADC, 1); // the carry is clear
    c.MemX(Mn::STA, kPercentAt);
    c.Mem(Mn::LDA, kPercentAt + 2);
    c.Imm(Mn::CMP, '0' | 0x80);
    c.Br(Mn::BEQ, cell);
    c.Imm(Mn::CMP, '5' | 0x80);
    c.Br(Mn::BNE, check);
    c.Place(cell);
    c.Imm(Mn::LDA, kFullCell);
    c.Place(fill);
    c.Mem(Mn::STA, kMeterAt);
    c.MemLabel(Mn::INC, fill, 1);
    c.Jump(Mn::JMP, check);
    c.Place(next);
    c.Op(Mn::DEY);
    c.Br(Mn::BNE, page);
}

struct LoaderLabels {
    std::int32_t seek = -1;
    std::int32_t address = -1;
    std::int32_t data = -1;
    std::int32_t to_aux = -1;
    std::int32_t table = -1;
    std::int32_t ret = -1;
    std::int32_t message = -1;
};

// each segment from the table: first page, pages and bank; 0 pages ends the table. the
// language card's bank 1 or bank 2 is switched in for writes (two reads of $C08B or $C083)
void EmitSegmentStart(Loader& l, const LoaderLabels& at, std::int32_t segment, std::int32_t track,
                      std::int32_t finish) {
    Code6502& c = l.c;
    std::int32_t go = c.Label("segment.go");
    std::int32_t card = c.Label("segment.card");
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
    c.Imm(Mn::CMP, static_cast<int>(LoaderBank::kCardBank1));
    c.Br(Mn::BNE, card);
    c.Mem(Mn::BIT, 0xC08B);
    c.Mem(Mn::BIT, 0xC08B);
    c.Place(card);
    c.Imm(Mn::CMP, static_cast<int>(LoaderBank::kCard));
    c.Br(Mn::BNE, track);
    c.Mem(Mn::LDA, 0xC083);
    c.Mem(Mn::LDA, 0xC083);
}

// the pages the loader reads, in every segment: the meter's 100%
uint32_t PlanPages(const LoaderPlan& plan) {
    uint32_t pages = 0;
    for (const LoaderSegment& segment : plan.segments) {
        pages += segment.pages;
    }
    return pages;
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
    c.Mem(Mn::STA, kMeter);
    c.Mem(Mn::STA, kMeter + 1);
    c.Imm(Mn::LDA, kBootFirstTrack);
    c.Mem(Mn::STA, kTrack);
    c.Op(Mn::TXA); // the latch reads: $C08C + slot * 16
    c.Imm(Mn::ORA, 0x8C);
    for (std::int32_t site : l.latch_reads) {
        c.MemLabel(Mn::STA, site, 1);
    }
    EmitScreen(l, at.message);
    EmitNibbleTable(l);
    EmitSegmentStart(l, at, segment, track, finish);
    c.Jump(Mn::JMP, track);
    c.Place(elsewhere); // the head is on another track: start from there
    c.OpA(Mn::ASL);
    c.Mem(Mn::STA, kHead);

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

    // a wanted sector to its page; an aux one by way of the bounce page
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
    c.Imm(Mn::LDA, l.bounce >> 8);
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
    EmitProgress(l, PlanPages(plan));
    EmitNextTrack(l); // every segment starts on a track of its own
    c.Mem(Mn::LDA, kLeft);
    c.Br(Mn::BNE, more);
    c.Jump(Mn::JMP, segment);
    c.Place(more);
    c.Jump(Mn::JMP, track);

    c.Place(finish);
    c.Mem(Mn::LDX, kSlot);
    c.MemX(Mn::LDA, 0xC088); // motor off
    c.Mem(Mn::LDA, 0xC082);  // language card: rom reads, no writes
    c.Imm(Mn::LDX, 0xFF);
    c.Op(Mn::TXS);
    if (plan.slot_at != 0) {
        c.Mem(Mn::LDA, kSlot);
        c.Mem(Mn::STA, plan.slot_at);
    }
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
    l.bounce = plan.bounce_page << 8;
    Code6502& c = l.c;
    LoaderLabels at;
    at.message = c.Label("message");
    at.seek = c.Label("seek");
    std::int32_t wait = c.Label("wait");
    std::int32_t nibble = c.Label("nibble");
    at.address = c.Label("address");
    at.data = c.Label("data");
    at.to_aux = c.Label("to_aux");
    at.table = c.Label("table");
    at.ret = c.Label("return");
    EmitSeek(l, at.seek, wait);
    EmitWait(l, wait);
    EmitNibble(l, nibble);
    EmitAddress(l, at.address, nibble);
    EmitData(l, at.data, nibble);
    EmitToAux(l, at.to_aux);
    c.Place(at.table);
    for (const LoaderSegment& segment : plan.segments) {
        c.Byte(segment.first_page);
        c.Byte(segment.pages);
        c.Byte(static_cast<int>(segment.bank));
    }
    c.Byte(0);
    c.Byte(0); // pages 0: the end
    c.Place(at.message);
    for (int i = 0; kLoading[i] != 0; i++) {
        c.Byte(kLoading[i] | 0x80);
    }
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
    // version 2 read aux sectors through text page 1; version 1 was two sectors and loaded
    // one stretch of main memory
    return ReadSignature(sectors[2], found[2], kLoaderVersion, return_pc) ||
           ReadSignature(sectors[2], found[2], 2, return_pc) ||
           ReadSignature(sectors[1], found[1], 1, return_pc);
}

} // namespace tisple
