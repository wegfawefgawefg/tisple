// const tables placed :on disk: their bytes go to Program::disk_tables, their var becomes the
// record lib/disk reads them by (disk/disk_tables.hpp, docs/disks.md)

#include "check/checker.hpp"
#include "disk/disk_tables.hpp"

namespace tisple {

bool MoveToDisk(Checker& ck, ConstDecl& cd, const Node& option) {
    Var& var = ck.out->vars[static_cast<std::size_t>(cd.var)];
    const Placement& place = var.placement;
    if (!place.on_disk) {
        Err(ck, option, ":interleave goes with :on disk; write :on disk :interleave 2");
        return false;
    }
    if (place.has_address || place.align != 0 || place.boundary != 0) {
        Err(ck, option,
            cd.name + " is :on disk, so it has no address in memory; drop :address, :align "
                      "and :boundary");
        return false;
    }
    const Type& type = TypeOf(*ck.out, var.type);
    if (type.kind != TypeKind::Array || type.elem != kU8Type) {
        Err(ck, option,
            "a table :on disk holds bytes; make " + cd.name +
                " an (array u8 n), as tables.packed_bytes does");
        return false;
    }
    if (!var.addr_slots.empty()) {
        Err(ck, option,
            cd.name + " holds addresses, which belong to memory, not the disk; keep it in "
                      "memory");
        return false;
    }
    if (type.size > kMaxDiskTableBytes) {
        std::string most = std::to_string(kMaxDiskTableBytes);
        Err(ck, option,
            cd.name + " is " + std::to_string(type.size) + " bytes; a table :on disk holds " +
                most + " at most, more than memory takes at once; split it");
        return false;
    }
    DiskTable table;
    table.var = cd.var;
    table.bytes = var.init;
    table.bytes.resize(type.size, 0);
    table.interleave = place.interleave != 0 ? place.interleave : kDefaultInterleave;
    std::uint32_t record = DiskTableRecordSize(type.size);
    TypeId record_type = ArrayOf(ck, kU8Type, record, option);
    Var& moved = ck.out->vars[static_cast<std::size_t>(cd.var)];
    moved.type = record_type;
    moved.init.assign(record, 0);
    cd.type = record_type;
    ck.out->disk_tables.push_back(table);
    return true;
}

void PlaceDiskConsts(Checker& ck) {
    Program& program = *ck.out;
    if (program.disk_tables.empty()) {
        return;
    }
    // declaration order: a const folded early (named in another's value) keeps its place
    std::vector<DiskTable> ordered;
    for (const ConstDecl& cd : ck.consts) {
        for (const DiskTable& table : program.disk_tables) {
            if (table.var == cd.var && cd.var >= 0) {
                ordered.push_back(table);
            }
        }
    }
    program.disk_tables = ordered;
    std::string error;
    if (!PlaceDiskTables(program.disk_tables, error)) {
        const Var& first = program.vars[static_cast<std::size_t>(program.disk_tables[0].var)];
        AddError(*ck.errors, first.loc, error);
        return;
    }
    for (const DiskTable& table : program.disk_tables) {
        program.vars[static_cast<std::size_t>(table.var)].init = DiskTableRecord(table);
    }
}

} // namespace tisple
