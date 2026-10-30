// the built program's pieces: segments of main, language card and aux memory, and reading
// and describing them (backend.hpp)

#include "backend/link.hpp"

#include <algorithm>
#include <cstdio>

namespace tisple {

MemorySegment SegmentOf(const std::vector<std::uint8_t>& memory, MemoryBank bank, std::uint32_t low,
                        std::uint32_t high) {
    MemorySegment segment;
    segment.bank = bank;
    segment.address = static_cast<std::uint16_t>(low);
    segment.bytes.assign(memory.begin() + low, memory.begin() + high);
    return segment;
}

// every :in aux let's bytes: runs of them, a new segment after a gap of a page or more
void AuxSegments(const Program& program, const Layout& layout, const std::vector<std::uint8_t>& aux,
                 std::vector<MemorySegment>& out) {
    std::vector<AddressRange> pieces;
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const Var& var = program.vars[v];
        const VarPlace& place = layout.vars[v];
        bool untouched = var.placement.has_address && var.init.empty();
        if (var.placement.in_aux && !untouched && place.size > 0) {
            pieces.push_back(AddressRange{place.address, place.address + place.size});
        }
    }
    std::sort(pieces.begin(), pieces.end(),
              [](const AddressRange& a, const AddressRange& b) { return a.start < b.start; });
    std::size_t i = 0;
    while (i < pieces.size()) {
        std::uint32_t low = pieces[i].start;
        std::uint32_t high = pieces[i].end;
        i++;
        while (i < pieces.size() && pieces[i].start < high + 0x100) {
            high = std::max(high, pieces[i].end);
            i++;
        }
        out.push_back(SegmentOf(aux, MemoryBank::kAux, low, high));
    }
}

std::uint8_t CompiledByte(const Compiled& compiled, std::uint16_t address, bool bank1) {
    std::uint32_t at = address - static_cast<std::uint32_t>(compiled.origin);
    if (address >= compiled.origin && at < compiled.image.size()) {
        return compiled.image[at];
    }
    for (const MemorySegment& segment : compiled.segments) {
        std::uint32_t offset = address - static_cast<std::uint32_t>(segment.address);
        bool memory =
            bank1 ? segment.bank == MemoryBank::kCardBank1
                  : segment.bank != MemoryBank::kAux && segment.bank != MemoryBank::kCardBank1;
        if (memory && address >= segment.address && offset < segment.bytes.size()) {
            return segment.bytes[offset];
        }
    }
    return 0;
}

std::vector<MemorySegment> ProgramSegments(const Compiled& compiled) {
    MemorySegment main;
    main.bank = MemoryBank::kMain;
    main.address = compiled.origin;
    main.bytes = compiled.image;
    std::vector<MemorySegment> out = {main};
    out.insert(out.end(), compiled.segments.begin(), compiled.segments.end());
    return out;
}

std::string DescribeSegments(const Compiled& c) {
    char text[200];
    std::snprintf(text, sizeof(text),
                  "%u bytes at $%04X (code %u, data %u); zero page %u bytes (frames %u), "
                  "frames in memory %u bytes\n",
                  static_cast<unsigned>(c.image.size()), c.origin, c.code_bytes, c.data_bytes,
                  c.zp_bytes, c.zp_frames, c.mem_frames);
    std::string out = text;
    for (const MemorySegment& segment : c.segments) {
        std::snprintf(text, sizeof(text), "  and %u bytes at $%04X in %s memory\n",
                      static_cast<unsigned>(segment.bytes.size()), segment.address,
                      MemoryBankName(segment.bank));
        out += text;
    }
    return out;
}

} // namespace tisple
