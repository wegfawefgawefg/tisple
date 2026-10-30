#include "reader/format_internal.hpp"
#include "reader/keywords.hpp"

namespace tisple {

namespace {

const std::string& Text(const Formatter& fm) {
    return fm.file->text;
}

bool IsHorizontalSpace(char c) {
    return IsSpaceChar(c) && c != '\n';
}

// two or more newlines between the previous token and offset
bool BlankBefore(const Formatter& fm, std::uint32_t offset) {
    const std::string& text = Text(fm);
    int newlines = 0;
    std::uint32_t p = offset;
    while (p > 0 && IsSpaceChar(text[p - 1])) {
        newlines += text[p - 1] == '\n' ? 1 : 0;
        p--;
    }
    return p > 0 && newlines >= 2;
}

// nothing but spaces between the start of the line and offset
bool StartsLine(const Formatter& fm, std::uint32_t offset) {
    const std::string& text = Text(fm);
    std::uint32_t p = offset;
    while (p > 0 && IsHorizontalSpace(text[p - 1])) {
        p--;
    }
    return p == 0 || text[p - 1] == '\n';
}

// offset of the last non-space character before offset
std::uint32_t PreviousChar(const Formatter& fm, std::uint32_t offset) {
    const std::string& text = Text(fm);
    std::uint32_t p = offset;
    while (p > 0 && IsSpaceChar(text[p - 1])) {
        p--;
    }
    return p - 1;
}

// next token start after offset, skipping spaces and comments; size at the end
std::uint32_t NextToken(const Formatter& fm, std::uint32_t offset) {
    const std::string& text = Text(fm);
    std::uint32_t p = offset;
    while (p < text.size()) {
        char c = text[p];
        if (IsSpaceChar(c)) {
            p++;
        } else if (c == ';') {
            while (p < text.size() && text[p] != '\n') {
                p++;
            }
        } else {
            break;
        }
    }
    return static_cast<std::uint32_t>(p);
}

// the token a trailing comment pushes onto a new line: the next one after any ')',
// unless another comment comes first (that comment already starts a new line)
std::uint32_t BrokenToken(const Formatter& fm, std::uint32_t offset) {
    const std::string& text = Text(fm);
    std::uint32_t p = offset;
    while (p < text.size() && (IsSpaceChar(text[p]) || text[p] == ')')) {
        p++;
    }
    if (p < text.size() && text[p] == ';') {
        return static_cast<std::uint32_t>(text.size());
    }
    return static_cast<std::uint32_t>(p);
}

const Node* Find(const std::unordered_map<std::uint32_t, const Node*>& map, std::uint32_t at) {
    auto it = map.find(at);
    return it == map.end() ? nullptr : it->second;
}

void Register(Formatter& fm, const Node& node, const Node* parent, std::uint32_t index) {
    NodeNotes& notes = fm.notes[&node];
    notes.parent = parent;
    notes.index = index;
    notes.blank_before = fm.keep_trivia && BlankBefore(fm, node.span.start);
    notes.line_before = fm.keep_trivia && StartsLine(fm, node.span.start);
    // pre-order: the outermost node starting at an offset, the innermost ending there
    // (',(a b)' and its list end together; a ')' belongs to the list)
    fm.by_start.emplace(node.span.start, &node);
    fm.by_end[node.span.end] = &node;
    for (std::uint32_t i = 0; i < node.items.size(); i++) {
        Register(fm, node.items[i], &node, i);
    }
}

void AttachOwnLine(Formatter& fm, std::uint32_t comment) {
    Span span = fm.tree->comments[comment];
    std::uint32_t next = NextToken(fm, span.end);
    if (next >= Text(fm).size()) {
        fm.top_dangling.push_back(comment);
        return;
    }
    if (Text(fm)[next] == ')') {
        const Node* list = Find(fm.by_end, next + 1);
        if (list) {
            fm.notes[list].dangling.push_back(comment);
        }
        return;
    }
    const Node* node = Find(fm.by_start, next);
    if (!node) {
        return;
    }
    NodeNotes& notes = fm.notes[node];
    if (notes.parent && notes.index == 0) {
        // before the head: keep it with the '(' line instead
        fm.notes[notes.parent].after_open.push_back(comment);
        return;
    }
    notes.leading.push_back(comment);
}

void AttachTrailing(Formatter& fm, std::uint32_t comment) {
    Span span = fm.tree->comments[comment];
    std::uint32_t prev = PreviousChar(fm, span.start);
    if (Text(fm)[prev] == '(') {
        const Node* list = Find(fm.by_start, prev);
        if (list) {
            fm.notes[list].after_open.push_back(comment);
        }
        return;
    }
    const Node* before = Find(fm.by_end, prev + 1);
    if (before) {
        fm.notes[before].trailing.push_back(comment);
    }
    // the line ended here, so the next real token starts a new line
    std::uint32_t next = BrokenToken(fm, span.end);
    const Node* after = Find(fm.by_start, next);
    if (after) {
        fm.notes[after].break_before = true;
    }
}

void ComputeFlat(Formatter& fm, const Node& node) {
    NodeNotes& notes = fm.notes[&node];
    if (node.kind != NodeKind::List) {
        notes.flat_ok = true;
        notes.flat_width = static_cast<std::uint32_t>(node.text.size());
        return;
    }
    for (const Node& item : node.items) {
        ComputeFlat(fm, item);
    }
    if (ShowsMark(node)) {
        const NodeNotes& inner = fm.notes[&node.items[1]];
        notes.flat_ok = inner.flat_ok;
        notes.flat_width = inner.flat_width +
                           static_cast<std::uint32_t>(std::string(QuoteMarkText(node.mark)).size());
        return;
    }

    // a match clause with several forms lays out as a body
    bool clause_body = IsMatchClause(fm, node) && node.items.size() > 2;
    bool ok = notes.dangling.empty() && !HasBody(node) && !clause_body;
    std::uint32_t width = 2;
    for (std::uint32_t i = 0; i < node.items.size(); i++) {
        const NodeNotes& item = fm.notes[&node.items[i]];
        ok = ok && item.flat_ok;
        if (i > 0) {
            ok = ok && !item.break_before && item.leading.empty();
            width++;
        }
        width += item.flat_width;
    }
    notes.flat_ok = ok;
    notes.flat_width = width;
}

bool HeadIs(const Node& node, const char* name) {
    return node.kind == NodeKind::List && node.mark == QuoteMark::None && !node.items.empty() &&
           node.items[0].kind == NodeKind::Symbol && node.items[0].text == name;
}

// written like a type: u8, Sprite, (ptr t), (array t n), (fn (t ...) r)
bool IsTypeForm(const Node& node) {
    if (node.kind == NodeKind::Symbol) {
        return ClassifySymbol(node.text) == SymbolClass::Type;
    }
    return HeadIs(node, "ptr") || HeadIs(node, "array") || HeadIs(node, "fn");
}

// (fn (u8 u16) void): parameter types and a return type, where a macro's (fn (a b) body)
// has names and a body
bool IsFnPointerType(const Node& node) {
    if (node.items.size() != 3 || node.items[1].mark != QuoteMark::None ||
        !IsTypeForm(node.items[2])) {
        return false;
    }
    for (const Node& param : node.items[1].items) {
        if (!IsTypeForm(param)) {
            return false;
        }
    }
    return true;
}

} // namespace

int BodyHeaderCount(const Node& node) {
    if (node.kind != NodeKind::List || node.mark != QuoteMark::None || node.items.empty() ||
        node.items[0].kind != NodeKind::Symbol) {
        return -1;
    }
    const std::string& head = node.items[0].text;
    if (head == "fn") {
        // (fn name (params) type body ...); in macro code (fn (params) body ...); a function
        // pointer type (fn (u8 u16) void) is no body form
        bool anonymous = node.items.size() > 1 && node.items[1].kind == NodeKind::List;
        if (anonymous && IsFnPointerType(node)) {
            return -1;
        }
        // (fn name (params) type :interrupt body ...): the option stays on the head line
        bool option =
            !anonymous && node.items.size() > 4 && node.items[4].kind == NodeKind::Keyword;
        return anonymous ? 1 : option ? 4 : 3;
    }
    if (head == "macro") {
        // (macro name (params) body ...)
        return 2;
    }
    if (head == "if" || head == "while" || head == "match") {
        return 1;
    }
    if (head == "for") {
        // (for i type start end body ...); macro code has no type: (for i start end body ...)
        bool typed = node.items.size() > 2 && node.items[2].kind == NodeKind::Symbol &&
                     ClassifySymbol(node.items[2].text) == SymbolClass::Type;
        return typed ? 4 : 3;
    }
    if (head == "do") {
        return 0;
    }
    return -1;
}

bool HasBody(const Node& node) {
    int header = BodyHeaderCount(node);
    return header >= 0 && node.items.size() > static_cast<std::size_t>(header) + 1;
}

int OnePerLineKeep(const Node& node) {
    if (HeadIs(node, "enum") || HeadIs(node, "struct")) {
        return 2;
    }
    return -1;
}

bool IsParamList(const Formatter& fm, const Node& node) {
    if (node.kind != NodeKind::List || node.mark != QuoteMark::None) {
        return false;
    }
    auto it = fm.notes.find(&node);
    if (it == fm.notes.end() || !it->second.parent || it->second.index != 2) {
        return false;
    }
    const Node& parent = *it->second.parent;
    return HeadIs(parent, "fn") || HeadIs(parent, "macro") || HeadIs(parent, "struct") ||
           HeadIs(parent, "enum");
}

bool IsMatchClause(const Formatter& fm, const Node& node) {
    if (node.kind != NodeKind::List || node.mark != QuoteMark::None || node.items.empty()) {
        return false;
    }
    auto it = fm.notes.find(&node);
    if (it == fm.notes.end() || !it->second.parent || it->second.index < 2) {
        return false;
    }
    return HeadIs(*it->second.parent, "match");
}

bool IsDeclaration(const Node& node) {
    return HeadIs(node, "let") || HeadIs(node, "const");
}

void BuildNotes(Formatter& fm) {
    const std::vector<Node>& forms = fm.tree->forms;
    for (std::uint32_t i = 0; i < forms.size(); i++) {
        Register(fm, forms[i], nullptr, i);
    }

    if (fm.keep_trivia) {
        const std::vector<Span>& comments = fm.tree->comments;
        fm.comment_blank_before.resize(comments.size());
        for (std::uint32_t i = 0; i < comments.size(); i++) {
            fm.comment_blank_before[i] = BlankBefore(fm, comments[i].start);
            if (StartsLine(fm, comments[i].start)) {
                AttachOwnLine(fm, i);
            } else {
                AttachTrailing(fm, i);
            }
        }
    }

    for (const Node& form : forms) {
        ComputeFlat(fm, form);
    }
}

} // namespace tisple
