#include "gaps/sites.hpp"

namespace tisple {

namespace {

const SiteInfo* InfoAt(const SiteMap& map, std::uint16_t address) {
    const SiteInfo& info = map.at[address];
    return info.item < 0 ? nullptr : &info;
}

// "a/b/c.tsp:12: text" becomes "c.tsp:12: text"
std::string WithoutFolders(const std::string& note) {
    std::size_t colon = note.find(':');
    if (colon == std::string::npos) {
        return note;
    }
    std::size_t slash = note.rfind('/', colon);
    if (slash == std::string::npos) {
        return note;
    }
    // keep the folder a program lives in: programs/life/main.tsp reads better as life/main.tsp
    std::size_t parent = slash == 0 ? std::string::npos : note.rfind('/', slash - 1);
    std::size_t from = parent == std::string::npos ? 0 : parent + 1;
    return note.substr(from);
}

} // namespace

void BuildSiteMap(const Compiled& compiled, SiteMap& out) {
    out.at.assign(65536, SiteInfo());
    const Assembly& as = compiled.as;
    for (std::size_t s = 0; s < compiled.spans.size(); s++) {
        const FunctionSpan& span = compiled.spans[s];
        std::int32_t note = -1;
        std::int32_t prev = -1;
        std::int32_t label = -1;
        for (std::size_t i = span.first; i < span.end; i++) {
            const AsmItem& item = as.items[i];
            if (item.kind == ItemKind::Note) {
                note = item.note;
                continue;
            }
            if (item.kind == ItemKind::Label) {
                prev = -1;
                const std::string& name = as.symbols[static_cast<std::size_t>(item.value.sym)].name;
                if (!name.empty() && name[0] != 'L') {
                    label = item.value.sym;
                }
                continue;
            }
            if (item.kind != ItemKind::Ins) {
                continue;
            }
            // a long branch is two instructions; both bytes of it map to the one item
            for (std::uint32_t b = 0; b < item.size; b++) {
                SiteInfo& info = out.at[static_cast<std::uint16_t>(item.address + b)];
                info.item = static_cast<std::int32_t>(i);
                info.span = static_cast<std::int32_t>(s);
                info.note = note;
                info.prev = prev;
                info.label = label;
            }
            prev = static_cast<std::int32_t>(i);
        }
    }
}

std::string SiteFunction(const Compiled& compiled, const SiteMap& map, std::uint16_t address) {
    const SiteInfo* info = InfoAt(map, address);
    if (info == nullptr || info->span < 0) {
        return "?";
    }
    const FunctionSpan& span = compiled.spans[static_cast<std::size_t>(info->span)];
    if (span.function >= 0 || info->label < 0 || span.name != "runtime") {
        return span.name;
    }
    // runtime routines' labels are routine.part: mul16.add, divu8.loop
    const std::string& label = compiled.as.symbols[static_cast<std::size_t>(info->label)].name;
    return span.name + " " + label.substr(0, label.find('.'));
}

std::string SiteSource(const Compiled& compiled, const SiteMap& map, std::uint16_t address) {
    const SiteInfo* info = InfoAt(map, address);
    if (info == nullptr || info->note < 0) {
        return "";
    }
    std::string text = WithoutFolders(compiled.as.notes[static_cast<std::size_t>(info->note)]);
    if (text.size() > 100) {
        text = text.substr(0, 97) + "...";
    }
    return text;
}

std::string SiteText(const Compiled& compiled, const SiteMap& map, std::uint16_t address) {
    const SiteInfo* info = InfoAt(map, address);
    if (info == nullptr) {
        return "?";
    }
    return ItemText(compiled.as, compiled.as.items[static_cast<std::size_t>(info->item)]);
}

std::string ShapeText(Mn mn, Mode mode) {
    std::string text = MnName(mn);
    switch (mode) {
    case Mode::Acc:
        return text + " A";
    case Mode::Imm:
        return text + " #n";
    case Mode::Zp:
        return text + " zp";
    case Mode::ZpX:
        return text + " zp,X";
    case Mode::ZpY:
        return text + " zp,Y";
    case Mode::Abs:
        return text + " abs";
    case Mode::AbsX:
        return text + " abs,X";
    case Mode::AbsY:
        return text + " abs,Y";
    case Mode::Ind:
        return text + " (abs)";
    case Mode::IndX:
        return text + " (zp,X)";
    case Mode::IndY:
        return text + " (zp),Y";
    default:
        return text;
    }
}

std::string ItemShape(const Compiled& compiled, std::int32_t item) {
    if (item < 0) {
        return "label";
    }
    const AsmItem& it = compiled.as.items[static_cast<std::size_t>(item)];
    return ShapeText(it.mn, EncodedMode(compiled.as, it));
}

} // namespace tisple
