#include "reader/format_internal.hpp"

#include <algorithm>

namespace tisple {

namespace {

void EmitNode(Formatter& fm, const Node& node, std::uint32_t base, std::uint32_t trailing);

NodeNotes& Notes(Formatter& fm, const Node& node) {
    return fm.notes[&node];
}

void Write(Formatter& fm, const std::string& text) {
    fm.out += text;
    fm.column += static_cast<std::uint32_t>(text.size());
}

std::string CommentText(const Formatter& fm, std::uint32_t comment) {
    Span span = fm.tree->comments[comment];
    std::string text = fm.file->text.substr(span.start, span.end - span.start);
    while (!text.empty() && IsSpaceChar(text.back())) {
        text.pop_back();
    }
    return text;
}

// ends the line: pending trailing comments go first, joined when several meet
// (a comment after '(' that moved to the end of a line that has its own)
void NewLine(Formatter& fm, std::uint32_t indent, bool blank) {
    if (fm.out.empty()) {
        return;
    }
    for (std::size_t i = 0; i < fm.pending.size(); i++) {
        if (i == 0) {
            fm.marks.push_back(TrailingMark{fm.line, fm.column});
        }
        fm.out += ' ';
        fm.out += CommentText(fm, fm.pending[i]);
    }
    fm.pending.clear();
    fm.out += blank ? "\n\n" : "\n";
    fm.line += blank ? 2 : 1;
    fm.out += std::string(indent, ' ');
    fm.column = indent;
}

// trailing comments on consecutive lines line up, as clang-format does
void AlignTrailingComments(Formatter& fm) {
    if (fm.marks.empty()) {
        return;
    }
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= fm.out.size()) {
        std::size_t end = fm.out.find('\n', start);
        if (end == std::string::npos) {
            end = fm.out.size();
        }
        lines.push_back(fm.out.substr(start, end - start));
        start = end + 1;
    }

    std::size_t run = 0;
    while (run < fm.marks.size()) {
        std::size_t run_end = run + 1;
        std::uint32_t widest = fm.marks[run].column;
        while (run_end < fm.marks.size() &&
               fm.marks[run_end].line == fm.marks[run_end - 1].line + 1) {
            widest = std::max(widest, fm.marks[run_end].column);
            run_end++;
        }
        // only when every comment still ends inside the column limit
        bool fits = true;
        for (std::size_t i = run; i < run_end; i++) {
            const TrailingMark& mark = fm.marks[i];
            std::size_t length = lines[mark.line].size() + widest - mark.column;
            fits = fits && length <= kFormatColumns;
        }
        for (std::size_t i = run; i < run_end && fits; i++) {
            const TrailingMark& mark = fm.marks[i];
            lines[mark.line].insert(mark.column, widest - mark.column, ' ');
        }
        run = run_end;
    }

    fm.out.clear();
    for (std::size_t i = 0; i < lines.size(); i++) {
        fm.out += lines[i];
        if (i + 1 < lines.size()) {
            fm.out += '\n';
        }
    }
}

void WriteComment(Formatter& fm, std::uint32_t comment) {
    Write(fm, CommentText(fm, comment));
}

void PushAll(Formatter& fm, const std::vector<std::uint32_t>& comments) {
    fm.pending.insert(fm.pending.end(), comments.begin(), comments.end());
}

bool NeedsOwnLine(Formatter& fm, const Node& node) {
    NodeNotes& notes = Notes(fm, node);
    return notes.break_before || !notes.leading.empty();
}

// fits at the current column (plus a space first when asked), ')' run included
bool Fits(Formatter& fm, const Node& node, std::uint32_t trailing, bool space) {
    NodeNotes& notes = Notes(fm, node);
    std::uint32_t start = fm.column + (space ? 1 : 0);
    return notes.flat_ok && start + notes.flat_width + trailing <= kFormatColumns;
}

// blank line above an item that starts a line: never right after a head line
enum class Blank : unsigned char {
    Never,
    Source,
    Force,
};

bool BlankFirst(Blank rule, bool source) {
    return rule == Blank::Force || (rule == Blank::Source && source);
}

// leading comments, then a new line for the node
void StartItemLine(Formatter& fm, const Node& node, std::uint32_t indent, Blank rule) {
    NodeNotes& notes = Notes(fm, node);
    bool first = true;
    for (std::uint32_t comment : notes.leading) {
        bool source = fm.comment_blank_before[comment];
        NewLine(fm, indent, first ? BlankFirst(rule, source) : source);
        WriteComment(fm, comment);
        first = false;
    }
    NewLine(fm, indent, first ? BlankFirst(rule, notes.blank_before) : notes.blank_before);
}

void EmitFlat(Formatter& fm, const Node& node) {
    NodeNotes& notes = Notes(fm, node);
    if (node.kind != NodeKind::List) {
        Write(fm, node.text);
    } else if (node.mark != QuoteMark::None && node.items.size() == 2) {
        Write(fm, QuoteMarkText(node.mark));
        EmitFlat(fm, node.items[1]);
    } else {
        Write(fm, "(");
        PushAll(fm, notes.after_open);
        for (std::size_t i = 0; i < node.items.size(); i++) {
            if (i > 0) {
                Write(fm, " ");
            }
            EmitFlat(fm, node.items[i]);
        }
        Write(fm, ")");
    }
    PushAll(fm, notes.trailing);
}

// ')' count after the last item: one more, unless dangling comments push ')' down
std::uint32_t LastTrailing(Formatter& fm, const Node& list, std::uint32_t trailing) {
    return Notes(fm, list).dangling.empty() ? trailing + 1 : 0;
}

std::uint32_t ItemTrailing(Formatter& fm, const Node& list, std::size_t index,
                           std::uint32_t trailing) {
    return index + 1 == list.items.size() ? LastTrailing(fm, list, trailing) : 0;
}

void Open(Formatter& fm, const Node& list, std::uint32_t base, std::uint32_t trailing) {
    Write(fm, "(");
    PushAll(fm, Notes(fm, list).after_open);
    EmitNode(fm, list.items[0], base + kFormatIndent, ItemTrailing(fm, list, 0, trailing));
}

void Close(Formatter& fm, const Node& list, std::uint32_t base) {
    NodeNotes& notes = Notes(fm, list);
    for (std::uint32_t comment : notes.dangling) {
        NewLine(fm, base + kFormatIndent, fm.comment_blank_before[comment]);
        WriteComment(fm, comment);
    }
    if (!notes.dangling.empty()) {
        NewLine(fm, base, false);
    }
    Write(fm, ")");
    PushAll(fm, notes.trailing);
}

// header items (fn name, if condition, enum name) stay on the head line
void EmitHeaderItem(Formatter& fm, const Node& list, std::size_t index, std::uint32_t base,
                    std::uint32_t trailing) {
    const Node& item = list.items[index];
    std::uint32_t item_base = base + kFormatIndent;
    if (NeedsOwnLine(fm, item)) {
        item_base = base + 2 * kFormatIndent;
        StartItemLine(fm, item, item_base, Blank::Never);
    } else {
        Write(fm, " ");
    }
    EmitNode(fm, item, item_base, ItemTrailing(fm, list, index, trailing));
}

// a keyword followed by its value is one unit: :align 16
std::size_t UnitSize(const Node& list, std::size_t index) {
    bool pair = list.items[index].kind == NodeKind::Keyword && index + 1 < list.items.size() &&
                list.items[index + 1].kind != NodeKind::Keyword;
    return pair ? 2 : 1;
}

// a unit on its own line at indent
void EmitUnitLine(Formatter& fm, const Node& list, std::size_t index, std::uint32_t indent,
                  std::uint32_t trailing, Blank rule) {
    const Node& item = list.items[index];
    StartItemLine(fm, item, indent, rule);
    if (UnitSize(list, index) == 1) {
        EmitNode(fm, item, indent, ItemTrailing(fm, list, index, trailing));
        return;
    }
    EmitNode(fm, item, indent, 0);
    const Node& value = list.items[index + 1];
    if (NeedsOwnLine(fm, value)) {
        StartItemLine(fm, value, indent + kFormatIndent, Blank::Never);
    } else {
        Write(fm, " ");
    }
    EmitNode(fm, value, indent, ItemTrailing(fm, list, index + 1, trailing));
}

void EmitBodyForm(Formatter& fm, const Node& list, std::uint32_t base, std::uint32_t trailing,
                  int header) {
    Open(fm, list, base, trailing);
    std::size_t count = list.items.size();
    std::size_t header_end = std::min<std::size_t>(count, static_cast<std::size_t>(header) + 1);
    for (std::size_t i = 1; i < header_end; i++) {
        EmitHeaderItem(fm, list, i, base, trailing);
    }
    for (std::size_t i = header_end; i < count; i++) {
        Blank rule = i == header_end ? Blank::Never : Blank::Source;
        StartItemLine(fm, list.items[i], base + kFormatIndent, rule);
        EmitNode(fm, list.items[i], base + kFormatIndent, ItemTrailing(fm, list, i, trailing));
    }
    Close(fm, list, base);
}

void EmitOnePerLine(Formatter& fm, const Node& list, std::uint32_t base, std::uint32_t trailing,
                    int keep) {
    Open(fm, list, base, trailing);
    std::size_t count = list.items.size();
    std::size_t keep_end = std::min<std::size_t>(count, static_cast<std::size_t>(keep) + 1);
    for (std::size_t i = 1; i < keep_end; i++) {
        EmitHeaderItem(fm, list, i, base, trailing);
    }
    for (std::size_t i = keep_end; i < count; i += UnitSize(list, i)) {
        Blank rule = i == keep_end ? Blank::Never : Blank::Source;
        EmitUnitLine(fm, list, i, base + kFormatIndent, trailing, rule);
    }
    Close(fm, list, base);
}

const Node& Unmarked(const Node& node) {
    if (node.mark != QuoteMark::None && node.items.size() == 2) {
        return Unmarked(node.items[1]);
    }
    return node;
}

// width of "(head header..." for a body form, or 0 when its header cannot sit flat
std::uint32_t BodyHeadWidth(Formatter& fm, const Node& node) {
    const Node& body = Unmarked(node);
    int header = BodyHeaderCount(body);
    std::uint32_t width = Notes(fm, node).flat_width - Notes(fm, body).flat_width + 1;
    for (int i = 0; i <= header && i < static_cast<int>(body.items.size()); i++) {
        const Node& item = body.items[static_cast<std::size_t>(i)];
        if (!Notes(fm, item).flat_ok || (i > 0 && NeedsOwnLine(fm, item))) {
            return 0;
        }
        width += Notes(fm, item).flat_width + (i > 0 ? 1 : 0);
    }
    return width;
}

// (let m (if c ...)): a body form as the last argument hangs off the head line
bool CanHang(Formatter& fm, const Node& list) {
    std::size_t count = list.items.size();
    const Node& last = list.items[count - 1];
    if (count < 2 || !HasBody(Unmarked(last)) || NeedsOwnLine(fm, last)) {
        return false;
    }
    std::uint32_t width = fm.column + 1;
    for (std::size_t i = 0; i + 1 < count; i++) {
        const Node& item = list.items[i];
        NodeNotes& notes = Notes(fm, item);
        if (!notes.flat_ok || item.kind == NodeKind::Keyword || (i > 0 && NeedsOwnLine(fm, item))) {
            return false;
        }
        width += notes.flat_width + 1;
    }
    std::uint32_t head = BodyHeadWidth(fm, last);
    return head > 0 && width + head <= kFormatColumns;
}

bool IsPlainAtom(const Node& node) {
    return node.kind != NodeKind::List && node.kind != NodeKind::Keyword;
}

// head and first argument stay; lists get their own lines; atoms fill lines
void EmitGeneral(Formatter& fm, const Node& list, std::uint32_t base, std::uint32_t trailing) {
    Open(fm, list, base, trailing);
    std::size_t count = list.items.size();
    std::uint32_t indent = base + kFormatIndent;
    if (count > 1 && CanHang(fm, list)) {
        for (std::size_t i = 1; i + 1 < count; i++) {
            Write(fm, " ");
            EmitFlat(fm, list.items[i]);
        }
        Write(fm, " ");
        EmitNode(fm, list.items[count - 1], base, LastTrailing(fm, list, trailing));
        Close(fm, list, base);
        return;
    }

    bool line_ends_in_atom = false;
    bool broke = false;
    for (std::size_t i = 1; i < count; i += UnitSize(list, i)) {
        const Node& item = list.items[i];
        std::uint32_t item_trailing = ItemTrailing(fm, list, i, trailing);
        bool plain = IsPlainAtom(item);
        bool same_line = false;
        if (UnitSize(list, i) == 1 && item.kind != NodeKind::Keyword && !NeedsOwnLine(fm, item) &&
            Fits(fm, item, item_trailing, true)) {
            same_line = i == 1 || (plain && line_ends_in_atom);
        }
        if (!same_line) {
            EmitUnitLine(fm, list, i, indent, trailing, broke ? Blank::Source : Blank::Never);
            broke = true;
            line_ends_in_atom = plain && UnitSize(list, i) == 1;
            continue;
        }
        Write(fm, " ");
        EmitNode(fm, item, indent, item_trailing);
        line_ends_in_atom = plain;
    }
    Close(fm, list, base);
}

void EmitNode(Formatter& fm, const Node& node, std::uint32_t base, std::uint32_t trailing) {
    if (node.kind != NodeKind::List) {
        EmitFlat(fm, node);
        return;
    }
    if (Fits(fm, node, trailing, false)) {
        EmitFlat(fm, node);
        return;
    }
    if (node.items.empty()) {
        // () holding only comments
        Write(fm, "(");
        PushAll(fm, Notes(fm, node).after_open);
        Close(fm, node, base);
        return;
    }
    if (node.mark != QuoteMark::None && node.items.size() == 2) {
        Write(fm, QuoteMarkText(node.mark));
        EmitNode(fm, node.items[1], base, trailing);
        PushAll(fm, Notes(fm, node).trailing);
        return;
    }
    int header = BodyHeaderCount(node);
    if (header >= 0) {
        EmitBodyForm(fm, node, base, trailing, header);
        return;
    }
    if (IsMatchClause(fm, node)) {
        EmitBodyForm(fm, node, base, trailing, 0);
        return;
    }
    if (IsParamList(fm, node)) {
        EmitOnePerLine(fm, node, base, trailing, 0);
        return;
    }
    int keep = OnePerLineKeep(node);
    if (keep >= 0) {
        EmitOnePerLine(fm, node, base, trailing, keep);
        return;
    }
    EmitGeneral(fm, node, base, trailing);
}

// top-level forms that take several lines get a blank line on both sides
bool TopIsMultiLine(Formatter& fm, const Node& form) {
    NodeNotes& notes = Notes(fm, form);
    return !notes.flat_ok || notes.flat_width > kFormatColumns;
}

void EmitTopLevel(Formatter& fm) {
    const std::vector<Node>& forms = fm.tree->forms;
    bool previous_multi = false;
    for (std::size_t i = 0; i < forms.size(); i++) {
        bool multi = TopIsMultiLine(fm, forms[i]);
        bool force = i > 0 && (previous_multi || multi);
        StartItemLine(fm, forms[i], 0, force ? Blank::Force : Blank::Source);
        EmitNode(fm, forms[i], 0, 0);
        previous_multi = multi;
    }
    for (std::uint32_t comment : fm.top_dangling) {
        NewLine(fm, 0, fm.comment_blank_before[comment]);
        WriteComment(fm, comment);
    }
    if (!fm.pending.empty()) {
        // flush trailing comments of the last line
        NewLine(fm, 0, false);
        fm.out.pop_back();
    }
    if (!fm.out.empty()) {
        fm.out += '\n';
    }
}

} // namespace

std::string FormatTree(const SourceFile& file, const Tree& tree, bool keep_trivia) {
    Formatter fm;
    fm.file = &file;
    fm.tree = &tree;
    fm.keep_trivia = keep_trivia;
    BuildNotes(fm);
    EmitTopLevel(fm);
    AlignTrailingComments(fm);
    return fm.out;
}

bool FormatSource(const SourceFile& file, std::string& out, ReadError& error) {
    Tree tree;
    if (!ReadTree(file, tree, error)) {
        return false;
    }
    out = FormatTree(file, tree, true);
    return true;
}

} // namespace tisple
