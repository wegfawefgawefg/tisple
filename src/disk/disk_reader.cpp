// lib/disk's reader: begin, next and their bookkeeping (disk_reader.hpp). License: MIT as well
// as GPL-3.0-or-later: it is copied into users' programs. Original to Tisple, built from the
// boot loader's routines; not derived from Apple's RWTS or any other project's.

#include "disk/reader_code.hpp"

namespace tisple {

namespace {

// begin: the arguments, the record's header, the tables once, the slot, the card's signature,
// the latch reads patched, motor on
void EmitBegin(Reader& r, const ReaderLabels& at) {
    Code6502& c = r.l.c;
    std::int32_t built = c.Label("begin.built");
    std::int32_t has_slot = c.Label("begin.has_slot");
    std::int32_t no_card = c.Label("begin.no_card");
    std::int32_t empty = c.Label("begin.empty");
    c.Place(at.begin);
    c.Mem(Mn::STX, r.Var(kRvMode));
    c.Mem(Mn::LDA, 0xFC);
    c.Mem(Mn::STA, r.Var(kRvDstLo));
    c.Mem(Mn::LDA, 0xFD);
    c.Mem(Mn::STA, r.Var(kRvDstHi));
    c.Imm(Mn::LDY, 0);
    c.IndY(Mn::LDA, 0xFA);
    c.Mem(Mn::STA, r.Var(kRvLenLo));
    c.Mem(Mn::STA, r.Var(kRvLast));
    c.Op(Mn::INY);
    c.IndY(Mn::LDA, 0xFA);
    c.Mem(Mn::STA, r.Var(kRvLenHi));
    c.Op(Mn::INY);
    c.IndY(Mn::LDA, 0xFA);
    c.Mem(Mn::STA, r.Var(kRvFirst));
    // pieces: the high byte, and one more for a part page
    c.Mem(Mn::LDA, r.Var(kRvLenLo));
    c.Imm(Mn::CMP, 1);
    c.Mem(Mn::LDA, r.Var(kRvLenHi));
    c.Imm(Mn::ADC, 0);
    c.Mem(Mn::STA, r.Var(kRvSectors));
    // the record's entries, from its fourth byte
    c.Mem(Mn::LDA, 0xFA);
    c.Op(Mn::CLC);
    c.Imm(Mn::ADC, 3);
    for (std::int32_t site : at.entry_reads) {
        c.MemLabel(Mn::STA, site, 1);
    }
    c.Mem(Mn::LDA, 0xFB);
    c.Imm(Mn::ADC, 0);
    for (std::int32_t site : at.entry_reads) {
        c.MemLabel(Mn::STA, site, 2);
    }
    c.Imm(Mn::LDA, 0);
    c.Mem(Mn::STA, r.Var(kRvEntry));
    c.Mem(Mn::STA, r.Var(kRvGot));
    c.Mem(Mn::STA, r.Var(kRvNeed));
    c.Mem(Mn::LDA, r.Var(kRvReady));
    c.Br(Mn::BNE, built);
    c.Jump(Mn::JSR, at.tables);
    c.Mem(Mn::INC, r.Var(kRvReady));
    c.Place(built);
    c.Mem(Mn::LDA, r.Var(kRvSlot));
    c.Br(Mn::BNE, has_slot);
    c.Imm(Mn::LDA, 0x60);
    c.Mem(Mn::STA, r.Var(kRvSlot));
    c.Place(has_slot);
    // a disk ii card's rom: $Cn01 = $20, $Cn03 = $00, $Cn05 = $03
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.Imm(Mn::ORA, 0xC0);
    for (std::int32_t site : at.signature) {
        c.MemLabel(Mn::STA, site, 2);
    }
    c.Place(at.signature[0]);
    c.Mem(Mn::LDA, 0xC601);
    c.Imm(Mn::CMP, 0x20);
    c.Br(Mn::BNE, no_card);
    c.Place(at.signature[1]);
    c.Mem(Mn::LDA, 0xC603);
    c.Br(Mn::BNE, no_card);
    c.Place(at.signature[2]);
    c.Mem(Mn::LDA, 0xC605);
    c.Imm(Mn::CMP, 0x03);
    c.Br(Mn::BNE, no_card);
    c.Mem(Mn::LDA, r.Var(kRvSlot)); // the latch reads: $C08C + slot * 16
    c.Imm(Mn::ORA, 0x8C);
    for (std::int32_t site : r.l.latch_reads) {
        c.MemLabel(Mn::STA, site, 1);
    }
    c.Mem(Mn::LDA, r.Var(kRvSectors));
    c.Br(Mn::BEQ, empty);
    c.Mem(Mn::LDX, r.Var(kRvSlot));
    c.MemX(Mn::LDA, 0xC089); // motor on, drive 1, read mode
    c.MemX(Mn::LDA, 0xC08A);
    c.MemX(Mn::LDA, 0xC08E);
    c.MemX(Mn::LDA, 0xC08C);
    c.Imm(Mn::LDA, kReaderMore);
    c.Op(Mn::RTS);
    c.Place(no_card);
    c.Imm(Mn::LDA, kReaderNoCard);
    c.Op(Mn::RTS);
    c.Place(empty);
    c.Imm(Mn::LDA, kReaderDone);
    c.Op(Mn::RTS);
}

// the pieces of the next track the record names: their physical sectors in want, the track
void EmitTrackSetup(Reader& r, const ReaderLabels& at) {
    Code6502& c = r.l.c;
    std::int32_t clear = c.Label("track.clear");
    std::int32_t loop = c.Label("track.loop");
    std::int32_t done = c.Label("track.done");
    c.Place(at.track_setup);
    c.Imm(Mn::LDX, 15);
    c.Imm(Mn::LDA, 0xFF);
    c.Place(clear);
    c.MemX(Mn::STA, r.Want());
    c.Op(Mn::DEX);
    c.Br(Mn::BPL, clear);
    c.Mem(Mn::LDY, r.Var(kRvEntry));
    c.Place(at.entry_reads[0]);
    c.MemY(Mn::LDA, 0x0300); // patched: the record's entries
    c.Imm(Mn::AND, 0xF0);
    c.Mem(Mn::STA, r.Var(kRvOffset));
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.Op(Mn::CLC);
    c.Mem(Mn::ADC, r.Var(kRvFirst));
    c.Mem(Mn::STA, r.Var(kRvTrack));
    c.Place(loop);
    c.Mem(Mn::CPY, r.Var(kRvSectors));
    c.Br(Mn::BEQ, done);
    c.Place(at.entry_reads[1]);
    c.MemY(Mn::LDA, 0x0300);
    c.Imm(Mn::AND, 0xF0);
    c.Mem(Mn::CMP, r.Var(kRvOffset));
    c.Br(Mn::BNE, done);
    c.Place(at.entry_reads[2]);
    c.MemY(Mn::LDA, 0x0300);
    c.Imm(Mn::AND, 0x0F);
    c.Op(Mn::TAX);
    c.Op(Mn::TYA);
    c.MemX(Mn::STA, r.Want());
    c.Mem(Mn::INC, r.Var(kRvNeed));
    c.Op(Mn::INY);
    c.Br(Mn::BNE, loop);
    c.Place(done);
    c.Mem(Mn::STY, r.Var(kRvEntry));
    c.Op(Mn::RTS);
}

// a piece in: the bounce page and the entries put together at dst + piece * 256, in aux
// memory with RAMWRT on and interrupts off; the last, part page through the bounce page so
// nothing past the table's end is written
void EmitPlace(Reader& r, const ReaderLabels& at) {
    Code6502& c = r.l.c;
    std::int32_t s0 = c.Label("fix.s0");
    std::int32_t s86 = c.Label("fix.s86");
    std::int32_t s172 = c.Label("fix.s172");
    std::int32_t t0 = c.Label("fix.t0");
    std::int32_t t86 = c.Label("fix.t86");
    std::int32_t whole = c.Label("place.whole");
    std::int32_t part = c.Label("place.part");
    std::int32_t carry86 = c.Label("place.86");
    std::int32_t carry172 = c.Label("place.172");
    std::int32_t to_main = c.Label("place.main");
    std::int32_t copy_go = c.Label("place.copy_go");
    std::int32_t copy = c.Label("place.copy");
    std::int32_t copy_to = c.Label("place.copy_to");
    c.Place(at.place);
    c.Mem(Mn::LDX, r.Var(kRvPiece));
    c.Op(Mn::INX);
    c.Mem(Mn::CPX, r.Var(kRvSectors));
    c.Br(Mn::BNE, whole);
    c.Mem(Mn::LDA, r.Var(kRvLast));
    c.Br(Mn::BNE, part);
    c.Place(whole);
    c.Mem(Mn::LDA, r.Var(kRvDstHi));
    c.Op(Mn::CLC);
    c.Mem(Mn::ADC, r.Var(kRvPiece));
    std::int32_t highs[] = {s0, s86, s172, t0, t86};
    for (std::int32_t site : highs) {
        c.MemLabel(Mn::STA, site, 2);
    }
    c.Mem(Mn::LDA, r.Var(kRvDstLo));
    c.MemLabel(Mn::STA, s0, 1);
    c.MemLabel(Mn::STA, t0, 1);
    c.Op(Mn::CLC);
    c.Imm(Mn::ADC, 86);
    c.MemLabel(Mn::STA, s86, 1);
    c.MemLabel(Mn::STA, t86, 1);
    c.Br(Mn::BCC, carry86);
    c.MemLabel(Mn::INC, s86, 2);
    c.MemLabel(Mn::INC, t86, 2);
    c.Place(carry86);
    c.Mem(Mn::LDA, r.Var(kRvDstLo));
    c.Op(Mn::CLC);
    c.Imm(Mn::ADC, 172);
    c.MemLabel(Mn::STA, s172, 1);
    c.Br(Mn::BCC, carry172);
    c.MemLabel(Mn::INC, s172, 2);
    c.Place(carry172);
    c.Mem(Mn::LDA, r.Var(kRvMode));
    c.Br(Mn::BEQ, to_main);
    c.Op(Mn::SEI);
    c.Mem(Mn::STA, 0xC005); // RAMWRT on: the stores go to aux memory
    c.Jump(Mn::JSR, at.fix);
    c.Mem(Mn::STA, 0xC004);
    r.RestoreI();
    c.Op(Mn::RTS);
    c.Place(to_main);
    c.Jump(Mn::JMP, at.fix);

    // the last page, part full: put together in the bounce page, then its bytes copied
    c.Place(part);
    c.Imm(Mn::LDA, r.bounce >> 8);
    for (std::int32_t site : highs) {
        c.MemLabel(Mn::STA, site, 2);
    }
    c.Imm(Mn::LDA, 0);
    c.MemLabel(Mn::STA, s0, 1);
    c.MemLabel(Mn::STA, t0, 1);
    c.Imm(Mn::LDA, 86);
    c.MemLabel(Mn::STA, s86, 1);
    c.MemLabel(Mn::STA, t86, 1);
    c.Imm(Mn::LDA, 172);
    c.MemLabel(Mn::STA, s172, 1);
    c.Jump(Mn::JSR, at.fix);
    c.Mem(Mn::LDA, r.Var(kRvDstLo));
    c.MemLabel(Mn::STA, copy_to, 1);
    c.Mem(Mn::LDA, r.Var(kRvDstHi));
    c.Op(Mn::CLC);
    c.Mem(Mn::ADC, r.Var(kRvPiece));
    c.MemLabel(Mn::STA, copy_to, 2);
    c.Mem(Mn::LDA, r.Var(kRvMode));
    c.Br(Mn::BEQ, copy_go);
    c.Op(Mn::SEI);
    c.Mem(Mn::STA, 0xC005);
    c.Place(copy_go);
    c.Imm(Mn::LDY, 0);
    c.Place(copy);
    c.MemY(Mn::LDA, r.bounce);
    c.Place(copy_to);
    c.MemY(Mn::STA, r.bounce); // patched
    c.Op(Mn::INY);
    c.Mem(Mn::CPY, r.Var(kRvLast));
    c.Br(Mn::BNE, copy);
    c.Mem(Mn::STA, 0xC004);
    r.RestoreI();
    c.Op(Mn::RTS);

    EmitReaderFix(r, at.fix, s0, s86, s172, t0, t86);
}

void EmitMotorOff(Reader& r) {
    r.l.c.Mem(Mn::LDX, r.Var(kRvSlot));
    r.l.c.MemX(Mn::LDA, 0xC088);
}

// next: a track's pieces chosen and the head on it, then the first wanted sector to pass
void EmitNext(Reader& r, const ReaderLabels& at) {
    Code6502& c = r.l.c;
    std::int32_t reading = c.Label("next.reading");
    std::int32_t new_track = c.Label("next.new_track");
    std::int32_t known = c.Label("next.known");
    std::int32_t recalibrate = c.Label("next.recalibrate");
    std::int32_t seek_track = c.Label("next.seek_track");
    std::int32_t loop = c.Label("next.loop");
    std::int32_t found = c.Label("next.found");
    std::int32_t skip = c.Label("next.skip");
    std::int32_t elsewhere = c.Label("next.elsewhere");
    std::int32_t more = c.Label("next.more");
    std::int32_t done = c.Label("next.done");
    std::int32_t no_disk = c.Label("next.no_disk");
    std::int32_t too_many = c.Label("next.too_many");
    std::int32_t bad_track = c.Label("next.bad_track");
    std::int32_t fail = c.Label("next.fail");
    std::int32_t leave = c.Label("next.leave");
    c.Place(at.next);
    c.Op(Mn::PHP);
    c.Op(Mn::PLA);
    c.Mem(Mn::STA, r.Var(kRvFlags));
    c.Op(Mn::CLD);
    c.Mem(Mn::LDA, r.Var(kRvNeed));
    c.Br(Mn::BNE, reading);
    c.Mem(Mn::LDA, r.Var(kRvEntry));
    c.Mem(Mn::CMP, r.Var(kRvSectors));
    c.Br(Mn::BCC, new_track);
    c.Jump(Mn::JMP, done);
    c.Place(new_track);
    c.Jump(Mn::JSR, at.track_setup);
    c.Imm(Mn::LDA, 0);
    c.Mem(Mn::STA, r.Var(kRvSeeks));
    c.Mem(Mn::LDA, r.Var(kRvHead));
    c.Imm(Mn::CMP, 0xFF);
    c.Br(Mn::BNE, known);
    // where the head is: an address field says, else from track 0 (80 steps out)
    c.Jump(Mn::JSR, at.address);
    c.Br(Mn::BCS, recalibrate);
    r.RestoreI();
    c.Mem(Mn::LDA, r.Var(kRvField + 2));
    c.OpA(Mn::ASL);
    c.Mem(Mn::STA, r.Var(kRvHead));
    c.Jump(Mn::JMP, known);
    c.Place(recalibrate);
    c.Imm(Mn::LDA, 80);
    c.Mem(Mn::STA, r.Var(kRvHead));
    c.Imm(Mn::LDA, 0);
    c.Jump(Mn::JSR, at.seek);
    c.Place(known);
    c.Place(seek_track);
    c.Mem(Mn::LDA, r.Var(kRvTrack));
    c.OpA(Mn::ASL);
    c.Jump(Mn::JSR, at.seek);

    c.Place(reading);
    c.Imm(Mn::LDA, 0);
    c.Mem(Mn::STA, r.Var(kRvFields));
    c.Place(loop);
    c.Jump(Mn::JSR, at.address);
    c.Br(Mn::BCC, found);
    c.Jump(Mn::JMP, no_disk);
    c.Place(found);
    c.Mem(Mn::INC, r.Var(kRvFields));
    c.Mem(Mn::LDA, r.Var(kRvFields));
    c.Imm(Mn::CMP, 48); // three turns
    c.Br(Mn::BCS, too_many);
    c.Mem(Mn::LDA, r.Var(kRvField + 2));
    c.Mem(Mn::CMP, r.Var(kRvTrack));
    c.Br(Mn::BNE, elsewhere);
    c.Mem(Mn::LDX, r.Var(kRvField + 1));
    c.Imm(Mn::CPX, 16);
    c.Br(Mn::BCS, skip);
    c.MemX(Mn::LDA, r.Want());
    c.Imm(Mn::CMP, 0xFF);
    c.Br(Mn::BEQ, skip);
    c.Jump(Mn::JSR, at.data);
    c.Br(Mn::BCS, skip);
    r.RestoreI();
    c.Mem(Mn::LDX, r.Var(kRvField + 1));
    c.MemX(Mn::LDA, r.Want());
    c.Mem(Mn::STA, r.Var(kRvPiece));
    c.Imm(Mn::LDA, 0xFF);
    c.MemX(Mn::STA, r.Want());
    c.Mem(Mn::DEC, r.Var(kRvNeed));
    c.Jump(Mn::JSR, at.place);
    c.Mem(Mn::INC, r.Var(kRvGot));
    c.Mem(Mn::LDA, r.Var(kRvGot));
    c.Mem(Mn::CMP, r.Var(kRvSectors));
    c.Br(Mn::BNE, more);
    c.Jump(Mn::JMP, done);
    c.Place(more);
    c.Imm(Mn::LDA, kReaderMore);
    c.Jump(Mn::JMP, leave);
    c.Place(skip);
    r.RestoreI();
    c.Jump(Mn::JMP, loop);
    c.Place(elsewhere); // the head is on another track: from there
    r.RestoreI();
    c.Mem(Mn::INC, r.Var(kRvSeeks));
    c.Mem(Mn::LDA, r.Var(kRvSeeks));
    c.Imm(Mn::CMP, 5);
    c.Br(Mn::BCS, bad_track);
    c.Mem(Mn::LDA, r.Var(kRvField + 2));
    c.OpA(Mn::ASL);
    c.Mem(Mn::STA, r.Var(kRvHead));
    c.Jump(Mn::JMP, seek_track);

    c.Place(done);
    EmitMotorOff(r);
    c.Imm(Mn::LDA, kReaderDone);
    c.Jump(Mn::JMP, leave);
    c.Place(too_many);
    r.RestoreI();
    c.Imm(Mn::LDA, kReaderBadSector);
    c.Br(Mn::BNE, fail);
    c.Place(bad_track);
    c.Imm(Mn::LDA, kReaderBadTrack);
    c.Br(Mn::BNE, fail);
    c.Place(no_disk);
    c.Imm(Mn::LDA, kReaderNoDisk);
    c.Place(fail); // motor off; later calls give done
    c.Mem(Mn::STA, r.Var(kRvResult));
    EmitMotorOff(r);
    c.Imm(Mn::LDA, 0);
    c.Mem(Mn::STA, r.Var(kRvNeed));
    c.Mem(Mn::LDA, r.Var(kRvSectors));
    c.Mem(Mn::STA, r.Var(kRvEntry));
    c.Mem(Mn::LDA, r.Var(kRvResult));
    c.Place(leave); // the caller's flags back, A the result
    c.Mem(Mn::STA, r.Var(kRvResult));
    r.RestoreI();
    c.Mem(Mn::LDA, r.Var(kRvResult));
    c.Op(Mn::RTS);
}

} // namespace

bool BuildDiskReader(std::uint16_t origin, std::vector<std::uint8_t>& out, std::string& error) {
    if ((origin & 0xFF) != 0 || origin < 0x0400 || origin > 0xBC00) {
        error = "the disk reader goes on a page from $0400 to $BC00";
        return false;
    }
    Reader r;
    r.origin = origin;
    r.bounce = origin - kReaderWork;
    r.page_a = r.bounce + 0x100;
    r.page_b = r.bounce + 0x200;
    r.l.at.slot = r.Var(kRvSlot);
    r.l.at.head = r.Var(kRvHead);
    r.l.at.target = r.Var(kRvTarget);
    r.l.at.next = r.Var(kRvNextHalf);
    r.l.at.wait = r.Var(kRvWait);
    Code6502& c = r.l.c;
    ReaderLabels at;
    at.begin = c.Label("begin");
    at.next = c.Label("next");
    at.seek = c.Label("seek");
    at.wait = c.Label("wait");
    at.address = c.Label("address");
    at.data = c.Label("data");
    at.fix = c.Label("fix");
    at.place = c.Label("place");
    at.tables = c.Label("tables");
    at.track_setup = c.Label("track_setup");
    for (int i = 0; i < 3; i++) {
        at.entry_reads[i] = c.Label("entry_read");
        at.signature[i] = c.Label("signature");
    }
    // the routines with latch reads first, since begin patches them
    EmitReaderAddress(r, at.address);
    EmitReaderData(r, at.data);
    EmitSeek(r.l, at.seek, at.wait);
    EmitWait(r.l, at.wait);
    EmitTrackSetup(r, at);
    EmitPlace(r, at);
    EmitReaderTables(r, at.tables);
    EmitNext(r, at);
    std::vector<AsmItem> routines = c.as.items;
    c.as.items.clear();
    c.Jump(Mn::JMP, at.begin);
    c.Jump(Mn::JMP, at.next);
    for (int i = kRvSlot; i < kRvCode; i++) {
        c.Byte(i == kRvHead ? 0xFF : 0);
    }
    EmitBegin(r, at);
    c.as.items.insert(c.as.items.end(), routines.begin(), routines.end());
    if (!BuildCode(c, origin, out, error)) {
        return false;
    }
    return true;
}

} // namespace tisple
