// lib/save's writer: begin, next, the sector's nibbles worked out, and the build
// (disk_writer.hpp). License: MIT as well as GPL-3.0-or-later: it is copied into users'
// programs. Original to Tisple, built from the boot loader's and lib/disk's reader's
// routines; not derived from Apple's RWTS or any other project's.

#include "disk/boot_loader.hpp"
#include "disk/nibble.hpp"
#include "disk/writer_code.hpp"

#include "disk/disk_reader.hpp"

#include <string>

namespace tisple {

namespace {

// begin: the record's header, the slot from lib/disk's reader and its card's signature, every
// track checked against the floor, the latch reads patched, motor on, the write protect
void EmitBegin(Writer& w, const WriterLabels& at) {
    Code6502& c = w.l.c;
    std::int32_t has_slot = c.Label("wbegin.has_slot");
    std::int32_t check = c.Label("wbegin.check");
    std::int32_t checked = c.Label("wbegin.checked");
    std::int32_t refused = c.Label("wbegin.refused");
    std::int32_t no_card = c.Label("wbegin.no_card");
    std::int32_t empty = c.Label("wbegin.empty");
    std::int32_t is_protected = c.Label("wbegin.protected");
    c.Place(at.begin);
    c.Op(Mn::CLD);
    c.Mem(Mn::LDA, 0xFC);
    c.Mem(Mn::STA, w.Var(kWvSrcLo));
    c.Mem(Mn::LDA, 0xFD);
    c.Mem(Mn::STA, w.Var(kWvSrcHi));
    c.Imm(Mn::LDY, 0);
    c.IndY(Mn::LDA, 0xFA); // pieces: the length's high byte, one more for a part page
    c.Imm(Mn::CMP, 1);
    c.Op(Mn::INY);
    c.IndY(Mn::LDA, 0xFA);
    c.Imm(Mn::ADC, 0);
    c.Mem(Mn::STA, w.Var(kWvPieces));
    c.Op(Mn::INY);
    c.IndY(Mn::LDA, 0xFA);
    c.Mem(Mn::STA, w.Var(kWvFirst));
    c.Mem(Mn::LDA, 0xFA); // the entries, from the record's fourth byte
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
    c.Mem(Mn::STA, w.Var(kWvPiece));
    c.Imm(Mn::LDA, 0xFF);
    c.Mem(Mn::STA, w.Var(kWvHead)); // the reader may have moved it: found again
    c.Mem(Mn::LDA, w.reader_slot);
    c.Br(Mn::BNE, has_slot);
    c.Imm(Mn::LDA, 0x60);
    c.Place(has_slot);
    c.Mem(Mn::STA, w.Var(kWvSlot));
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
    // every piece's track: the floor up to 34, never 17 (the catalog)
    c.Imm(Mn::LDY, 0);
    c.Place(check);
    c.Mem(Mn::CPY, w.Var(kWvPieces));
    c.Br(Mn::BEQ, checked);
    c.Place(at.entry_reads[0]);
    c.MemY(Mn::LDA, 0x0300); // patched: the record's entries
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.Op(Mn::CLC);
    c.Mem(Mn::ADC, w.Var(kWvFirst));
    c.Br(Mn::BCS, refused);
    c.Imm(Mn::CMP, w.floor);
    c.Br(Mn::BCC, refused);
    c.Imm(Mn::CMP, kDiskTracks);
    c.Br(Mn::BCS, refused);
    c.Imm(Mn::CMP, kBootCatalogTrack);
    c.Br(Mn::BEQ, refused);
    c.Op(Mn::INY);
    c.Br(Mn::BNE, check);
    c.Place(checked);
    c.Mem(Mn::LDA, w.Var(kWvPieces));
    c.Br(Mn::BEQ, empty);
    c.Mem(Mn::LDA, w.Var(kWvSlot)); // the latch reads: $C08C + slot * 16
    c.Imm(Mn::ORA, 0x8C);
    for (std::int32_t site : w.l.latch_reads) {
        c.MemLabel(Mn::STA, site, 1);
    }
    c.Mem(Mn::LDX, w.Var(kWvSlot));
    c.MemX(Mn::LDA, 0xC089); // motor on, drive 1
    c.MemX(Mn::LDA, 0xC08A);
    c.MemX(Mn::LDA, 0xC08D); // Q6 on, Q7 off: bit 7 is the write protect
    c.MemX(Mn::LDA, 0xC08E);
    c.Br(Mn::BMI, is_protected);
    c.MemX(Mn::LDA, 0xC08C); // read mode
    c.Imm(Mn::LDA, kWriterMore);
    c.Op(Mn::RTS);
    c.Place(is_protected);
    c.MemX(Mn::LDA, 0xC08C);
    c.MemX(Mn::LDA, 0xC088);
    c.Imm(Mn::LDA, kWriterProtected);
    c.Op(Mn::RTS);
    c.Place(refused);
    c.Imm(Mn::LDA, kWriterRefused);
    c.Op(Mn::RTS);
    c.Place(no_card);
    c.Imm(Mn::LDA, kWriterNoCard);
    c.Op(Mn::RTS);
    c.Place(empty);
    c.Imm(Mn::LDA, kWriterDone);
    c.Op(Mn::RTS);
}

void EmitMotorOff(Writer& w) {
    w.l.c.Mem(Mn::LDX, w.Var(kWvSlot));
    w.l.c.MemX(Mn::LDA, 0xC088);
}

// next: the piece's nibbles, the head to its track, its sector's address field, the write
void EmitNext(Writer& w, const WriterLabels& at) {
    Code6502& c = w.l.c;
    std::int32_t go = c.Label("wnext.go");
    std::int32_t known = c.Label("wnext.known");
    std::int32_t recalibrate = c.Label("wnext.recalibrate");
    std::int32_t seek_track = c.Label("wnext.seek_track");
    std::int32_t on_track = c.Label("wnext.on_track");
    std::int32_t loop = c.Label("wnext.loop");
    std::int32_t found = c.Label("wnext.found");
    std::int32_t skip = c.Label("wnext.skip");
    std::int32_t elsewhere = c.Label("wnext.elsewhere");
    std::int32_t more = c.Label("wnext.more");
    std::int32_t done = c.Label("wnext.done");
    std::int32_t no_disk = c.Label("wnext.no_disk");
    std::int32_t too_many = c.Label("wnext.too_many");
    std::int32_t bad_track = c.Label("wnext.bad_track");
    std::int32_t fail = c.Label("wnext.fail");
    std::int32_t leave = c.Label("wnext.leave");
    c.Place(at.next);
    c.Op(Mn::PHP);
    c.Op(Mn::PLA);
    c.Mem(Mn::STA, w.Var(kWvFlags));
    c.Op(Mn::CLD);
    c.Mem(Mn::LDA, w.Var(kWvPiece));
    c.Mem(Mn::CMP, w.Var(kWvPieces));
    c.Br(Mn::BCC, go);
    c.Jump(Mn::JMP, done);
    c.Place(go);
    c.Mem(Mn::LDY, w.Var(kWvPiece));
    c.Place(at.entry_reads[1]);
    c.MemY(Mn::LDA, 0x0300); // patched: the record's entries
    c.Op(Mn::PHA);
    c.Imm(Mn::AND, 0x0F);
    c.Mem(Mn::STA, w.Var(kWvSector));
    c.Op(Mn::PLA);
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.OpA(Mn::LSR);
    c.Op(Mn::CLC);
    c.Mem(Mn::ADC, w.Var(kWvFirst));
    c.Mem(Mn::STA, w.Var(kWvTrack));
    // the piece's bytes: the first piece's address plus 256 a piece
    c.Mem(Mn::LDA, w.Var(kWvSrcLo));
    for (std::int32_t site : at.source) {
        c.MemLabel(Mn::STA, site, 1);
    }
    c.Mem(Mn::LDA, w.Var(kWvSrcHi));
    c.Op(Mn::CLC);
    c.Mem(Mn::ADC, w.Var(kWvPiece));
    for (std::int32_t site : at.source) {
        c.MemLabel(Mn::STA, site, 2);
    }
    c.Mem(Mn::LDA, w.Var(kWvSrcLo)); // and less one
    c.Op(Mn::SEC);
    c.Imm(Mn::SBC, 1);
    c.MemLabel(Mn::STA, at.source_back, 1);
    c.Mem(Mn::LDA, w.Var(kWvSrcHi));
    c.Imm(Mn::SBC, 0);
    c.Op(Mn::CLC);
    c.Mem(Mn::ADC, w.Var(kWvPiece));
    c.MemLabel(Mn::STA, at.source_back, 2);
    c.Jump(Mn::JSR, at.prenibble);
    c.Imm(Mn::LDA, 0);
    c.Mem(Mn::STA, w.Var(kWvSeeks));
    c.Mem(Mn::LDA, w.Var(kWvHead));
    c.Imm(Mn::CMP, 0xFF);
    c.Br(Mn::BNE, known);
    // where the head is: an address field says, else from track 0 (80 steps out)
    c.Jump(Mn::JSR, at.address);
    c.Br(Mn::BCS, recalibrate);
    w.RestoreI();
    c.Mem(Mn::LDA, w.Var(kWvField + 2));
    c.OpA(Mn::ASL);
    c.Mem(Mn::STA, w.Var(kWvHead));
    c.Jump(Mn::JMP, known);
    c.Place(recalibrate);
    c.Imm(Mn::LDA, 80);
    c.Mem(Mn::STA, w.Var(kWvHead));
    c.Imm(Mn::LDA, 0);
    c.Jump(Mn::JSR, at.seek);
    c.Place(known);
    c.Place(seek_track);
    c.Mem(Mn::LDA, w.Var(kWvTrack));
    c.OpA(Mn::ASL);
    c.Mem(Mn::CMP, w.Var(kWvHead)); // there already: no seek, and no 25 ms to settle
    c.Br(Mn::BEQ, on_track);
    c.Jump(Mn::JSR, at.seek);
    c.Place(on_track);
    c.Imm(Mn::LDA, 0);
    c.Mem(Mn::STA, w.Var(kWvFields));
    c.Place(loop);
    c.Jump(Mn::JSR, at.address);
    c.Br(Mn::BCC, found);
    c.Jump(Mn::JMP, no_disk);
    c.Place(found);
    c.Mem(Mn::INC, w.Var(kWvFields));
    c.Mem(Mn::LDA, w.Var(kWvFields));
    c.Imm(Mn::CMP, 64); // four turns
    c.Br(Mn::BCS, too_many);
    c.Mem(Mn::LDA, w.Var(kWvField + 2));
    c.Mem(Mn::CMP, w.Var(kWvTrack));
    c.Br(Mn::BNE, elsewhere);
    c.Mem(Mn::LDA, w.Var(kWvField + 1));
    c.Mem(Mn::CMP, w.Var(kWvSector));
    c.Br(Mn::BNE, skip);
    c.Jump(Mn::JSR, at.field);
    w.RestoreI();
    c.Mem(Mn::INC, w.Var(kWvPiece));
    c.Mem(Mn::LDA, w.Var(kWvPiece));
    c.Mem(Mn::CMP, w.Var(kWvPieces));
    c.Br(Mn::BNE, more);
    c.Jump(Mn::JMP, done);
    c.Place(more);
    c.Imm(Mn::LDA, kWriterMore);
    c.Jump(Mn::JMP, leave);
    c.Place(skip);
    w.RestoreI();
    c.Jump(Mn::JMP, loop);
    c.Place(elsewhere); // the head is on another track: from there
    w.RestoreI();
    c.Mem(Mn::INC, w.Var(kWvSeeks));
    c.Mem(Mn::LDA, w.Var(kWvSeeks));
    c.Imm(Mn::CMP, 5);
    c.Br(Mn::BCS, bad_track);
    c.Mem(Mn::LDA, w.Var(kWvField + 2));
    c.OpA(Mn::ASL);
    c.Mem(Mn::STA, w.Var(kWvHead));
    c.Jump(Mn::JMP, seek_track);

    c.Place(done);
    EmitMotorOff(w);
    c.Imm(Mn::LDA, kWriterDone);
    c.Jump(Mn::JMP, leave);
    c.Place(too_many);
    w.RestoreI();
    c.Imm(Mn::LDA, kWriterBadSector);
    c.Br(Mn::BNE, fail);
    c.Place(bad_track);
    c.Imm(Mn::LDA, kWriterBadTrack);
    c.Br(Mn::BNE, fail);
    c.Place(no_disk);
    c.Imm(Mn::LDA, kWriterNoDisk);
    c.Place(fail); // motor off; later calls give done
    c.Mem(Mn::STA, w.Var(kWvResult));
    EmitMotorOff(w);
    c.Mem(Mn::LDA, w.Var(kWvPieces));
    c.Mem(Mn::STA, w.Var(kWvPiece));
    c.Mem(Mn::LDA, w.Var(kWvResult));
    c.Place(leave); // the caller's flags back, A the result
    c.Mem(Mn::STA, w.Var(kWvResult));
    w.RestoreI();
    c.Mem(Mn::LDA, w.Var(kWvResult));
    c.Op(Mn::RTS);
}

// the tables: the 64 data nibbles in order, and a byte's two low bits swapped, shifted 0, 2
// and 4
void EmitTables(Writer& w, const WriterLabels& at) {
    Code6502& c = w.l.c;
    c.Place(at.nibbles);
    for (int v = 0; v < 64; v++) {
        c.Byte(NibbleOf6(v));
    }
    c.Place(at.swaps);
    const int swapped[4] = {0, 2, 1, 3};
    for (int shift = 0; shift < 6; shift += 2) {
        for (int low = 0; low < 4; low++) {
            c.Byte(swapped[low] << shift);
        }
    }
}

// whether every branch from items first to end lands in its own page (a taken branch that
// crosses one takes a cycle more, and the write counts every cycle)
bool TimedBranchesStay(const Code6502& c, std::size_t first, std::size_t end) {
    for (std::size_t i = first; i < end; i++) {
        const AsmItem& item = c.as.items[i];
        if (item.kind != ItemKind::Ins || item.mode != Mode::Rel) {
            continue;
        }
        std::uint16_t after = static_cast<std::uint16_t>(item.address + 2);
        std::uint16_t target = static_cast<std::uint16_t>(
            c.as.symbols[static_cast<std::size_t>(item.value.sym)].value + item.value.offset);
        if ((after & 0xFF00) != (target & 0xFF00)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool BuildDiskWriter(std::uint16_t origin, int floor, std::uint16_t reader,
                     std::vector<std::uint8_t>& out, std::string& error) {
    if ((origin & 0xFF) != 0 || origin < 0x0400 || origin > 0xBA00) {
        error = "the disk writer goes on a page from $0400 to $BA00";
        return false;
    }
    if (floor < 1 || floor >= kDiskTracks) {
        error = "the disk writer's floor is a track from 1 to 34, not " + std::to_string(floor);
        return false;
    }
    Writer w;
    w.origin = origin;
    w.floor = floor;
    w.reader_slot = reader + kReaderSlot;
    w.high = reader - kReaderWork;
    w.l.at.slot = w.Var(kWvSlot);
    w.l.at.head = w.Var(kWvHead);
    w.l.at.target = w.Var(kWvTarget);
    w.l.at.next = w.Var(kWvNextHalf);
    w.l.at.wait = w.Var(kWvWait);
    Code6502& c = w.l.c;
    WriterLabels at;
    at.begin = c.Label("begin");
    at.next = c.Label("next");
    at.seek = c.Label("seek");
    at.wait = c.Label("wait");
    at.address = c.Label("address");
    at.field = c.Label("field");
    at.prenibble = c.Label("prenibble");
    at.nibbles = c.Label("nibbles");
    at.swaps = c.Label("swaps");
    for (int i = 0; i < 2; i++) {
        at.entry_reads[i] = c.Label("entry_read");
    }
    for (int i = 0; i < 3; i++) {
        at.signature[i] = c.Label("signature");
    }
    for (std::int32_t& site : at.source) {
        site = c.Label("source");
    }
    at.source_back = c.Label("source_back");
    // the routines with latch reads first, since begin patches them; the data field's
    // apart, since it goes where none of its timed branches crosses a page
    EmitWriterField(w, at.field);
    std::vector<AsmItem> field = c.as.items;
    c.as.items.clear();
    EmitWriterAddress(w, at.address);
    EmitSeek(w.l, at.seek, at.wait);
    EmitWait(w.l, at.wait);
    EmitWriterPrenibble(w, at);
    EmitNext(w, at);
    std::vector<AsmItem> routines = c.as.items;
    c.as.items.clear();
    EmitBegin(w, at);
    std::vector<AsmItem> begin = c.as.items;
    c.as.items.clear();
    EmitTables(w, at);
    std::vector<AsmItem> tables = c.as.items;
    bool stays = false;
    for (int filler = 0; filler < 256 && !stays; filler++) {
        c.as.items.clear();
        c.Jump(Mn::JMP, at.begin);
        c.Jump(Mn::JMP, at.next);
        for (int i = kWvSlot; i < kWvCode; i++) {
            c.Byte(i == kWvHead ? 0xFF : 0);
        }
        for (int i = 0; i < filler; i++) {
            c.Byte(0);
        }
        std::size_t field_start = c.as.items.size();
        c.as.items.insert(c.as.items.end(), field.begin(), field.end());
        std::size_t field_end = c.as.items.size();
        c.as.items.insert(c.as.items.end(), begin.begin(), begin.end());
        c.as.items.insert(c.as.items.end(), routines.begin(), routines.end());
        c.as.items.insert(c.as.items.end(), tables.begin(), tables.end());
        out.clear();
        if (!BuildCode(c, origin, out, error)) {
            return false;
        }
        stays = TimedBranchesStay(c, field_start, field_end);
    }
    if (!stays) {
        error = "the disk writer's data field has a branch across a page";
        return false;
    }
    if (out.size() > static_cast<std::size_t>(kWriterSum)) {
        error = "the disk writer's code is " + std::to_string(out.size()) + " bytes; it has " +
                std::to_string(kWriterSum) + " before its buffer";
        return false;
    }
    return true;
}

} // namespace tisple
