#include "check/name_index.hpp"

namespace tisple {

namespace {

// the node's text is exactly what the file holds at its span
bool WrittenAsIs(const Sources& sources, std::uint32_t file, const Node& node) {
    if (file >= sources.files.size()) {
        return false;
    }
    const std::string& text = sources.files[file].text;
    std::uint32_t length = node.span.end - node.span.start;
    if (node.span.end > text.size() || length != node.text.size() || length == 0) {
        return false;
    }
    return text.compare(node.span.start, length, node.text) == 0;
}

} // namespace

void NoteNameUse(NameIndex* index, const Sources& sources, std::uint32_t file, const Node& node,
                 std::size_t segment, NameUseKind kind, std::int32_t target, std::int32_t member,
                 bool declaration) {
    if (!index || target < 0) {
        return;
    }
    if (node.kind != NodeKind::Symbol && node.kind != NodeKind::Keyword) {
        return;
    }
    if (!WrittenAsIs(sources, file, node)) {
        return;
    }
    // :x names the field x; a symbol's segments are split at dots
    std::uint32_t start = node.kind == NodeKind::Keyword ? 1 : 0;
    for (std::size_t i = 0; i < segment; i++) {
        std::size_t dot = node.text.find('.', start);
        if (dot == std::string::npos) {
            return;
        }
        start = static_cast<std::uint32_t>(dot + 1);
    }
    std::size_t end = node.text.find('.', start);
    if (end == std::string::npos) {
        end = node.text.size();
    }
    if (end <= start) {
        return;
    }
    NameUse use;
    use.kind = kind;
    use.loc.file = file;
    use.loc.span = Span{node.span.start + start, node.span.start + static_cast<std::uint32_t>(end)};
    use.index = target;
    use.member = member;
    use.declaration = declaration;
    index->uses.push_back(use);
}

} // namespace tisple
