// placement options on a top-level let or const: :address, :align, :boundary, :in aux and
// :reserve, and :on disk, :interleave and :strings aux on a const table (docs/language.md,
// Placement)

#include "check/checker.hpp"

namespace tisple {

namespace {

bool FoldOption(Checker& ck, const Node& node, const std::string& label, std::uint32_t& out) {
    ExprId value = CheckExpr(ck, node, kU16Type);
    std::uint16_t raw = 0;
    if (!RequireType(ck, node, value, kU16Type, label)) {
        return false;
    }
    if (!FoldExpr(ck, value, raw)) {
        Err(ck, node, label + " needs a constant number, like 0x6000 or 256");
        return false;
    }
    out = raw;
    return true;
}

// :in names a region besides main memory: aux memory, or the language card's bank 1
void ReadIn(Checker& ck, Var& var, const Node& key, const Node* value) {
    bool known = value && value->kind == NodeKind::Symbol &&
                 (value->text == "aux" || value->text == "bank1");
    if (!known) {
        Err(ck, value ? *value : key,
            ":in takes aux (aux memory) or bank1 (the language card's bank 1); write :in aux");
        return;
    }
    if (var.placement.in_aux || var.placement.in_bank1) {
        Err(ck, key, ":in is given twice; keep one");
        return;
    }
    (value->text == "aux" ? var.placement.in_aux : var.placement.in_bank1) = true;
}

// :strings aux: the strings written in a const table's value live in aux memory
void ReadStrings(Checker& ck, Var& var, const Node& key, const Node* value) {
    if (!value || value->kind != NodeKind::Symbol || value->text != "aux") {
        Err(ck, value ? *value : key, ":strings takes aux; write :strings aux");
        return;
    }
    if (var.placement.strings_aux) {
        Err(ck, key, ":strings is given twice; keep one");
        return;
    }
    var.placement.strings_aux = true;
}

// :on names the disk the table's bytes live on, not in memory (docs/disks.md): disk, the boot
// disk, or disk2, the second disk (in drive 2, or swapped into drive 1)
void ReadOn(Checker& ck, Var& var, const Node& key, const Node* value) {
    bool known = value && value->kind == NodeKind::Symbol &&
                 (value->text == "disk" || value->text == "disk2");
    if (!known) {
        Err(ck, value ? *value : key, ":on takes disk or disk2; write :on disk");
        return;
    }
    if (var.placement.on_disk) {
        Err(ck, key, ":on is given twice; keep one");
        return;
    }
    var.placement.on_disk = true;
    var.placement.disk = value->text == "disk2" ? 2 : 1;
}

void ReadInterleave(Checker& ck, Var& var, const Node& key, const Node* value) {
    std::uint32_t number = 0;
    if (!value) {
        Err(ck, key, ":interleave needs a value; write :interleave 2");
        return;
    }
    if (!FoldOption(ck, *value, ":interleave", number)) {
        return;
    }
    if (var.placement.interleave != 0) {
        Err(ck, key, ":interleave is given twice; keep one");
        return;
    }
    if (number < 1 || number > 15) {
        Err(ck, *value,
            ":interleave is how many physical sectors apart a table's sectors go, 1 to 15; "
            "write :interleave 2");
        return;
    }
    var.placement.interleave = static_cast<std::uint8_t>(number);
}

void ReadNumberOption(Checker& ck, Var& var, const Node& key, const Node& value) {
    const std::string& name = key.text;
    std::uint32_t number = 0;
    if (!FoldOption(ck, value, name, number)) {
        return;
    }
    Placement& place = var.placement;
    bool repeated = (name == ":address" && place.has_address) ||
                    (name == ":align" && place.align != 0) ||
                    (name == ":boundary" && place.boundary != 0);
    if (repeated) {
        Err(ck, key, name + " is given twice; keep one");
        return;
    }
    if (name == ":address") {
        place.has_address = true;
        place.address = static_cast<std::uint16_t>(number);
        return;
    }
    if (number == 0) {
        Err(ck, value, name + " 0 means nothing; use 1 or more, like " + name + " 256");
        return;
    }
    if (name == ":align") {
        place.align = number;
    } else {
        place.boundary = number;
    }
}

} // namespace

// one option at key; gives how many items it took: :reserve stands alone, the rest take a value
int ReadOption(Checker& ck, Var& var, const Node& key, const Node* value) {
    const std::string& name = key.text;
    if (name == ":reserve") {
        if (var.placement.reserve) {
            Err(ck, key, ":reserve is given twice; keep one");
        }
        var.placement.reserve = true;
        return 1;
    }
    if (name == ":in") {
        ReadIn(ck, var, key, value);
        return 2;
    }
    if (name == ":on") {
        ReadOn(ck, var, key, value);
        return 2;
    }
    if (name == ":strings") {
        ReadStrings(ck, var, key, value);
        return 2;
    }
    if (name == ":interleave") {
        ReadInterleave(ck, var, key, value);
        return 2;
    }
    if (name != ":address" && name != ":align" && name != ":boundary") {
        Err(ck, key,
            name + " is not a placement option; use :address, :align, :boundary, :in aux, "
                   ":in bank1, :reserve, or on a const table :on disk, :interleave and "
                   ":strings aux");
        return 2;
    }
    if (!value) {
        Err(ck, key, name + " needs a value; write " + name + " 256");
        return 2;
    }
    ReadNumberOption(ck, var, key, *value);
    return 2;
}

namespace {

// :in aux at an :address: it must be memory RAMRD and RAMWRT switch
void CheckAuxAddress(Checker& ck, const Node& form, const Var& var) {
    const Placement& place = var.placement;
    if (!place.in_aux || !place.has_address || var.type <= kErrorType) {
        return;
    }
    std::uint32_t end = place.address + TypeOf(*ck.out, var.type).size;
    if (place.address < 0x0200 || end > 0xC000) {
        Err(ck, form,
            form.items[1].text +
                " is in aux memory at an :address outside $0200-$BFFF, which lib/auxmem "
                "cannot reach; move it");
    }
}

// :in bank1: room the link places at the top of the card's bank 1, which the startup never
// writes, so no value; and no other placement: the link picks its address
void CheckBank1Options(Checker& ck, const Node& form, const Var& var, bool has_value) {
    const Placement& place = var.placement;
    if (!place.in_bank1) {
        return;
    }
    const std::string& name = form.items[1].text;
    if (has_value) {
        Err(ck, form,
            name + " is :in bank1, which the startup never writes, so it has no value; fill "
                   "it while the program runs (lib/disk reads into it)");
    }
    if (place.has_address || place.align != 0 || place.boundary != 0 || place.reserve) {
        Err(ck, form,
            name + " is :in bank1, placed by the link at the top of the bank; drop :address, "
                   ":align, :boundary and :reserve");
    }
    if (var.type > kErrorType && TypeOf(*ck.out, var.type).size > 0x1000) {
        Err(ck, form, name + " is bigger than the language card's bank 1 (4096 bytes)");
    }
}

} // namespace

// :reserve is room at a fixed address with nothing in it; :in aux lives where lib/auxmem reaches
void CheckRegionOptions(Checker& ck, const Node& form, const Var& var, bool has_value) {
    const Placement& place = var.placement;
    const std::string& name = form.items[1].text;
    if (place.strings_aux) {
        Err(ck, form,
            ":strings aux is for const tables, whose strings are written in their value; make " +
                name + " a const");
    }
    if (place.reserve && !place.has_address) {
        Err(ck, form,
            ":reserve keeps memory at a fixed address; give it one: (let " + name +
                " (array u8 8192) :address 0x2000 :reserve)");
    }
    if (place.on_disk || place.interleave != 0) {
        Err(ck, form,
            std::string(place.on_disk ? ":on disk" : ":interleave") +
                " is for const tables, whose bytes never change; make " + name + " a const");
    }
    if (place.reserve && has_value) {
        Err(ck, form,
            name + " is :reserve, memory the program reaches by address, so it has no value; "
                   "drop the value or :reserve");
    }
    CheckAuxAddress(ck, form, var);
    CheckBank1Options(ck, form, var, has_value);
}

bool CheckConstOptions(Checker& ck, const Node& form, const Node& option, const Var& var) {
    const Placement& place = var.placement;
    const std::string& name = form.items[1].text;
    if (place.reserve) {
        Err(ck, option, ":reserve is for top-level lets; make " + name + " a let");
        return false;
    }
    if (place.in_bank1) {
        Err(ck, option,
            ":in bank1 is for lets with no value, filled while the program runs; make " + name +
                " a let, or put the table :in aux");
        return false;
    }
    // :in aux with :on disk: the bytes on the disk, the record in aux memory (a program copies
    // it to main memory for lib/disk, which reads records there)
    if (place.in_aux && place.strings_aux) {
        Err(ck, option,
            ":in aux puts " + name + "'s strings in aux memory already; drop :strings aux");
        return false;
    }
    std::size_t before = ck.errors->size();
    CheckAuxAddress(ck, form, var);
    return ck.errors->size() == before;
}

// placement options come last, as on let: (const T (array u8 3) (make ...) :align 256)
const Node* SplitOptions(Checker& ck, const Node& form, Node& trimmed, Var& holder) {
    std::size_t first = form.items.size();
    for (std::size_t i = 2; i < form.items.size(); i++) {
        if (form.items[i].kind == NodeKind::Keyword) {
            first = i;
            break;
        }
    }
    trimmed = form;
    trimmed.items.resize(first);
    for (std::size_t i = first; i < form.items.size();) {
        const Node& key = form.items[i];
        if (key.kind != NodeKind::Keyword) {
            Err(ck, key, "placement options come last, after the value");
            i++;
            continue;
        }
        const Node* value = i + 1 < form.items.size() ? &form.items[i + 1] : nullptr;
        i += static_cast<std::size_t>(ReadOption(ck, holder, key, value));
    }
    return first < form.items.size() ? &form.items[first] : nullptr;
}

} // namespace tisple
