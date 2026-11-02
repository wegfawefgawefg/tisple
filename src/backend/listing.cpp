#include "backend/backend.hpp"

#include <cstdio>

namespace tisple {

namespace {

std::string Hex4(std::uint32_t value) {
    char text[8];
    std::snprintf(text, sizeof(text), "%04X", value & 0xFFFF);
    return text;
}

// a byte of an overlay group's code: in its image in aux memory, at its offset in the region
std::uint8_t GroupByte(const Compiled& c, std::int32_t group, std::uint16_t address) {
    const OverlayImage& image = c.overlays.groups[static_cast<std::size_t>(group)];
    if (image.on_disk) {
        const std::vector<std::uint8_t>& bytes =
            c.overlays.disk_tables[static_cast<std::size_t>(image.disk_table)].bytes;
        std::uint32_t offset = address - static_cast<std::uint32_t>(c.overlays.region);
        return offset < bytes.size() ? bytes[offset] : 0;
    }
    std::uint32_t at = image.aux + (address - static_cast<std::uint32_t>(c.overlays.region));
    for (const MemorySegment& segment : c.segments) {
        std::uint32_t offset = at - static_cast<std::uint32_t>(segment.address);
        if (segment.bank == MemoryBank::kAux && at >= segment.address &&
            offset < segment.bytes.size()) {
            return segment.bytes[offset];
        }
    }
    return 0;
}

std::string BytesText(const Compiled& c, const AsmItem& item, const FunctionSpan& span) {
    std::string text;
    for (std::uint32_t i = 0; i < item.size && i < 3; i++) {
        char byte[4];
        std::uint16_t at = static_cast<std::uint16_t>(item.address + i);
        std::uint8_t value = span.overlay_group >= 0 ? GroupByte(c, span.overlay_group, at)
                                                     : CompiledByte(c, at, span.card_bank1);
        std::snprintf(byte, sizeof(byte), "%02X ", value);
        text += byte;
    }
    if (item.size > 3) {
        text += "..";
    }
    while (text.size() < 10) {
        text += ' ';
    }
    return text;
}

// bytes from item `at` up to the next note or the end of the span
std::uint32_t BytesUntilNote(const Assembly& as, std::size_t at, std::size_t end) {
    std::uint32_t total = 0;
    for (std::size_t i = at + 1; i < end && as.items[i].kind != ItemKind::Note; i++) {
        total += as.items[i].size;
    }
    return total;
}

} // namespace

std::string ListProgram(const Program& program, const Compiled& c) {
    const Assembly& as = c.as;
    std::string text;
    for (const Diagnostic& warning : c.warnings) {
        text += "; " + FormatDiagnostic(program.sources, warning) + "\n";
    }
    for (const FunctionSpan& span : c.spans) {
        std::uint32_t bytes = 0;
        std::uint16_t start = 0;
        bool started = false;
        for (std::size_t i = span.first; i < span.end; i++) {
            const AsmItem& item = as.items[i];
            if (!started && (item.kind == ItemKind::Ins || item.kind == ItemKind::Byte)) {
                start = item.address;
                started = true;
            }
            bytes += item.size;
        }
        if (bytes == 0) {
            continue;
        }
        std::string where = span.card_bank1 ? " in the language card's bank 1" : "";
        if (span.overlay_group >= 0) {
            const OverlayImage& image =
                c.overlays.groups[static_cast<std::size_t>(span.overlay_group)];
            where = " in overlay group " + std::to_string(span.overlay_group) +
                    (image.on_disk ? std::string(" (read from the second disk)")
                                   : " (kept at aux $" + Hex4(image.aux) + ")");
        }
        text += "\n" + span.name + "  $" + Hex4(start) + ", " + std::to_string(bytes) + " bytes" +
                where + "\n";
        for (std::size_t i = span.first; i < span.end; i++) {
            const AsmItem& item = as.items[i];
            if (item.kind == ItemKind::Note) {
                std::uint32_t count = BytesUntilNote(as, i, span.end);
                text += "    ; " + as.notes[static_cast<std::size_t>(item.note)] + "  [" +
                        std::to_string(count) + " bytes]\n";
                continue;
            }
            if (item.kind == ItemKind::Label) {
                const AsmSymbol& sym = as.symbols[static_cast<std::size_t>(item.value.sym)];
                if (sym.name.empty() || sym.name[0] != 'L') {
                    text += "  " + sym.name + ":\n";
                } else {
                    text += "  " + Hex4(item.address) + ":\n";
                }
                continue;
            }
            text +=
                "  " + Hex4(item.address) + "  " + BytesText(c, item, span) + ItemText(as, item);
            int cycles = ItemCycles(as, item);
            if (cycles > 0) {
                text += "    (" + std::to_string(cycles) + ")";
            }
            text += "\n";
        }
    }
    return text;
}

} // namespace tisple
