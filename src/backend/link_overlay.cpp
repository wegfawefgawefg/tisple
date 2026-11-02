// The link's part of the overlay groups: link_overlay.hpp.

#include "backend/link_overlay.hpp"

#include "disk/boot_loader.hpp"
#include "disk/disk_tables.hpp"
#include "disk/nibble.hpp"

namespace tisple {

void RouteOverlayCode(Backend& be, const LinkInput& in, const OverlayGroups& groups,
                      std::vector<AsmItem>& startup, std::vector<AsmItem>& runtime,
                      std::vector<AsmItem>& irq, OverlayCode& ov) {
    const Program& program = *be.program;
    std::vector<std::vector<AsmItem>*> pieces = {&startup, &runtime, &irq};
    std::vector<std::int32_t> piece_group = {-1, -1, -1};
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if ((*in.kept)[f]) {
            pieces.push_back(&be.code[f]);
            piece_group.push_back(groups.group[f]);
        }
    }
    // the groups read from the second disk: a root marked :overlay_disk2, in a boot disk build
    // whose program reads disk 2 (lib/disk's disk2_bit says where it is) and has tables there
    std::vector<bool> on_disk(groups.roots.size(), false);
    std::int32_t disk2_bit = -1;
    bool disk2 = false;
    for (const DiskTable& table : program.disk_tables) {
        disk2 = disk2 || table.disk == 2;
    }
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        if (program.vars[v].name == "disk.disk2_bit" && program.vars[v].function < 0) {
            disk2_bit = be.var_sym[v];
        }
    }
    if (in.options->disk2_images && disk2 && disk2_bit >= 0) {
        for (std::size_t g = 0; g < groups.roots.size(); g++) {
            for (std::int32_t r : groups.roots[g]) {
                on_disk[g] =
                    on_disk[g] || program.functions[static_cast<std::size_t>(r)].overlay_disk2;
            }
        }
    }
    RouteOverlays(be, groups, pieces, piece_group, on_disk, disk2_bit, ov);
}

RegionSpans AppendGroupSpans(Backend& be, const OverlayGroups& groups, Compiled& out) {
    const Program& program = *be.program;
    RegionSpans region;
    region.first = out.spans.size();
    for (std::size_t g = 0; g < groups.members.size(); g++) {
        for (std::int32_t f : groups.members[g]) {
            std::size_t i = static_cast<std::size_t>(f);
            AppendSpan(be.as.items, be.code[i], out, f, program.functions[i].name);
            out.spans.back().overlay_group = static_cast<std::int32_t>(g);
        }
    }
    region.end = out.spans.size();
    return region;
}

bool PlaceGroupImages(Backend& be, const Program& program, const Layout& layout,
                      const std::vector<AddressRange>& aux_reserved, const RegionSpans& region,
                      const OverlayGroups& groups, const OverlayCode& ov, Compiled& out,
                      std::vector<AddressRange>& images, std::vector<Diagnostic>& errors) {
    const std::vector<AsmItem>& items = be.as.items;
    std::vector<AddressRange> taken = aux_reserved;
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const VarPlace& place = layout.vars[v];
        if (program.vars[v].placement.in_aux && place.size > 0) {
            taken.push_back(AddressRange{place.address, place.address + place.size});
        }
    }
    std::vector<std::uint32_t> sizes(groups.members.size(), 0);
    for (std::size_t s = region.first; s < region.end; s++) {
        sizes[static_cast<std::size_t>(out.spans[s].overlay_group)] +=
            SpanSize(items, out.spans[s]);
    }
    std::uint32_t total = 0;
    for (std::size_t g = 0; g < sizes.size(); g++) {
        total += ov.on_disk[g] ? 0 : sizes[g];
    }
    // the second disk's next free slot, after the program's own tables (PlaceDisk2Tables)
    int slot = kDisk2FirstTrack * kSectorsPerTrack;
    for (const DiskTable& table : program.disk_tables) {
        if (table.disk == 2) {
            slot +=
                static_cast<int>(DiskTableSectors(static_cast<std::uint32_t>(table.bytes.size())));
        }
    }
    std::uint32_t at = 0;
    if (!PlaceOverlayImages(taken, total, at)) {
        AddError(errors, Loc(),
                 "no room in aux memory for the overlay groups' " + std::to_string(total) +
                     " bytes");
        out.overlay_aux_short = true;
        return false;
    }
    DefineSymbol(be.as, ov.region_sym, region.at);
    out.overlays.region = static_cast<std::uint16_t>(region.at);
    out.overlays.region_size = region.size;
    for (std::size_t g = 0; g < sizes.size(); g++) {
        OverlayImage image;
        image.functions = groups.members[g];
        image.roots = groups.roots[g];
        image.bytes = sizes[g];
        // its own stubs and every other group's
        std::size_t stubs = 0;
        for (const std::vector<std::int32_t>& roots : groups.roots) {
            stubs += roots.size();
        }
        image.load_cycles = OverlayLoadCycles(sizes[g], stubs);
        if (ov.on_disk[g]) {
            // a table on the second disk after the ones before it; its bytes come with the code
            DiskTable table;
            table.disk = 2;
            for (std::uint32_t k = 0; k < DiskTableSectors(sizes[g]); k++, slot++) {
                table.tracks.push_back(static_cast<std::uint8_t>(slot / kSectorsPerTrack));
                table.sectors.push_back(
                    static_cast<std::uint8_t>(Disk2Sector(slot % kSectorsPerTrack)));
            }
            if (slot > (kBootLastTrack + 1) * kSectorsPerTrack) {
                AddError(errors, Loc(),
                         "no room on the second disk for overlay group " + std::to_string(g) +
                             "'s " + std::to_string(sizes[g]) + " bytes");
                return false;
            }
            table.bytes.assign(sizes[g], 0);
            std::vector<std::uint8_t> record = DiskTableRecord(table);
            for (std::size_t i = 0; i < 4; i++) {
                DefineSymbol(be.as, ov.rec_sym[4 * g + i], record[i]);
            }
            DefineSymbol(be.as, ov.image_sym[g], 0);
            DefineSymbol(be.as, ov.size_sym[g], 0);
            image.on_disk = true;
            image.disk_table = static_cast<std::int32_t>(out.overlays.disk_tables.size());
            image.load_cycles = OverlayDiskCycles(sizes[g]) + OverlayLoadCycles(0, stubs);
            out.overlays.disk_tables.push_back(table);
            images.push_back(AddressRange{0, 0});
            out.overlays.groups.push_back(image);
            continue;
        }
        for (std::size_t i = 0; i < 4; i++) {
            DefineSymbol(be.as, ov.rec_sym[4 * g + i], 0);
        }
        DefineSymbol(be.as, ov.image_sym[g], at);
        // the low byte whole pages, the high byte the bytes after them
        DefineSymbol(be.as, ov.size_sym[g], (sizes[g] >> 8) | ((sizes[g] & 0xFF) << 8));
        images.push_back(AddressRange{at, at + sizes[g]});
        image.aux = static_cast<std::uint16_t>(at);
        out.overlays.groups.push_back(image);
        at += sizes[g];
    }
    for (const FunctionSpan& span : out.spans) {
        if (span.name == kOverlayStubsSpan || span.name == kOverlayCopySpan) {
            out.overlays.code_bytes += SpanSize(items, span);
        }
    }
    return true;
}

std::vector<std::int32_t> ItemGroups(std::size_t items, const std::vector<FunctionSpan>& spans,
                                     const RegionSpans& region) {
    std::vector<std::int32_t> out(items, -1);
    for (std::size_t s = region.first; s < region.end && s < spans.size(); s++) {
        for (std::size_t n = spans[s].first; n < spans[s].end; n++) {
            out[n] = spans[s].overlay_group;
        }
    }
    return out;
}

std::vector<std::uint16_t> OverlayAuxSwitches(const std::vector<AsmItem>& items,
                                              const std::vector<FunctionSpan>& spans) {
    std::vector<std::uint16_t> out;
    for (const FunctionSpan& span : spans) {
        for (std::size_t i = span.first; i < span.end && span.name == kOverlayCopySpan; i++) {
            const AsmItem& item = items[i];
            std::uint32_t address = static_cast<std::uint32_t>(item.value.offset);
            bool ramrd = address == 0xC002 || address == 0xC003;
            if (item.kind == ItemKind::Ins && item.value.sym < 0 && item.mode == Mode::Abs &&
                ramrd) {
                out.push_back(item.address);
            }
        }
    }
    return out;
}

} // namespace tisple
