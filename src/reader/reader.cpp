#include "reader/reader.hpp"

namespace tisple {

namespace {

struct Frame {
    Node node;
    bool is_mark = false; // waits for exactly one form
};

struct Reader {
    const SourceFile* file = nullptr;
    Tree* tree = nullptr;
    bool recover = false; // editors: note problems and read on
    std::vector<ReadError>* errors = nullptr;
    std::uint32_t last_end = 0; // end of the last token read
    std::vector<Frame> stack;
    std::uint32_t open_parens = 0;
    // first '(' at column 1 seen inside an unclosed form: a hint for missing ')'
    bool has_column_one_open = false;
    std::uint32_t column_one_open = 0;
    bool has_last_top = false;
    Span last_top;
};

std::string Text(const Reader& rd, Span span) {
    return rd.file->text.substr(span.start, span.end - span.start);
}

std::string DecodeString(const std::string& raw) {
    // raw keeps its quotes and was checked by the lexer
    std::string out;
    for (std::size_t i = 1; i + 1 < raw.size(); i++) {
        char c = raw[i];
        if (c != '\\') {
            out += c;
            continue;
        }
        char e = raw[i + 1];
        i++;
        if (e == 'n') {
            out += '\n';
        } else if (e == 't') {
            out += '\t';
        } else if (e == 'r') {
            out += '\r';
        } else if (e == '0') {
            out += '\0';
        } else if (e == 'x') {
            out += static_cast<char>(std::stoi(raw.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += e;
        }
    }
    return out;
}

QuoteMark MarkFromToken(TokenKind kind) {
    switch (kind) {
    case TokenKind::Quote:
        return QuoteMark::Quote;
    case TokenKind::Quasiquote:
        return QuoteMark::Quasiquote;
    case TokenKind::Unquote:
        return QuoteMark::Unquote;
    case TokenKind::UnquoteSplicing:
        return QuoteMark::UnquoteSplicing;
    default:
        return QuoteMark::None;
    }
}

// a finished form goes to the innermost open list; marks close as soon as they get one
void AddForm(Reader& rd, Node node) {
    while (!rd.stack.empty() && rd.stack.back().is_mark) {
        Node mark = std::move(rd.stack.back().node);
        rd.stack.pop_back();
        mark.span.end = node.span.end;
        mark.items.push_back(std::move(node));
        node = std::move(mark);
    }
    if (rd.stack.empty()) {
        // remember the last form written at column 1; an extra ')' likely closed it early
        bool column_one = PosFromOffset(*rd.file, node.span.start).column == 1;
        if (column_one || !rd.has_last_top) {
            rd.has_last_top = true;
            rd.last_top = node.span;
        }
        rd.tree->forms.push_back(std::move(node));
        return;
    }
    rd.stack.back().node.items.push_back(std::move(node));
}

void OpenList(Reader& rd, const Token& token) {
    if (rd.open_parens > 0 && !rd.has_column_one_open &&
        PosFromOffset(*rd.file, token.span.start).column == 1) {
        rd.has_column_one_open = true;
        rd.column_one_open = token.span.start;
    }
    Frame frame;
    frame.node.kind = NodeKind::List;
    frame.node.span = token.span;
    rd.stack.push_back(std::move(frame));
    rd.open_parens++;
}

bool CloseList(Reader& rd, const Token& token, ReadError& error) {
    if (rd.stack.empty()) {
        error.offset = token.span.start;
        error.message = "')' has no matching '('";
        if (rd.has_last_top) {
            // an extra ')' usually means the form before it closed too early
            error.has_note = true;
            error.note_offset = rd.last_top.end - 1;
            SourcePos start = PosFromOffset(*rd.file, rd.last_top.start);
            error.note = "the form from line " + std::to_string(start.line) + " closed here";
        }
        return false;
    }
    Node node = std::move(rd.stack.back().node);
    rd.stack.pop_back();
    node.span.end = token.span.end;
    rd.open_parens--;
    if (rd.open_parens == 0) {
        rd.has_column_one_open = false;
    }
    AddForm(rd, std::move(node));
    return true;
}

void OpenMark(Reader& rd, const Token& token) {
    Frame frame;
    frame.is_mark = true;
    frame.node.kind = NodeKind::List;
    frame.node.span = token.span;
    frame.node.mark = MarkFromToken(token.kind);
    Node head;
    head.kind = NodeKind::Symbol;
    head.span = token.span;
    head.text = QuoteMarkName(frame.node.mark);
    frame.node.items.push_back(std::move(head));
    rd.stack.push_back(std::move(frame));
}

Node AtomFromToken(const Reader& rd, const Token& token) {
    Node node;
    node.span = token.span;
    node.text = Text(rd, token.span);
    if (token.kind == TokenKind::Symbol) {
        node.kind = NodeKind::Symbol;
    } else if (token.kind == TokenKind::Keyword) {
        node.kind = NodeKind::Keyword;
    } else if (token.kind == TokenKind::Integer) {
        node.kind = NodeKind::Integer;
        node.value = token.value;
    } else {
        node.kind = NodeKind::String;
        node.bytes = DecodeString(node.text);
    }
    return node;
}

void FailUnclosed(Reader& rd, ReadError& error) {
    // the outermost unclosed '(' is the one a missing ')' left open
    std::uint32_t unclosed = 0;
    bool found = false;
    for (const Frame& frame : rd.stack) {
        if (!frame.is_mark) {
            if (!found) {
                unclosed = frame.node.span.start;
            }
            found = true;
        }
    }
    error.offset = unclosed;
    error.message = "this '(' is never closed";
    if (rd.has_column_one_open && rd.column_one_open > unclosed) {
        error.has_note = true;
        error.note_offset = rd.column_one_open;
        error.note = "this form starts at column 1 inside it; a ')' is probably missing before";
    }
}

// recovery: every open form ends at the last token read; a mark with no form yet is dropped
void CloseAllOpen(Reader& rd) {
    while (!rd.stack.empty()) {
        Frame frame = std::move(rd.stack.back());
        rd.stack.pop_back();
        if (frame.is_mark) {
            continue;
        }
        frame.node.span.end = rd.last_end;
        AddForm(rd, std::move(frame.node));
    }
    rd.open_parens = 0;
    rd.has_column_one_open = false;
}

bool AtColumnOne(const Reader& rd, const Token& token) {
    return PosFromOffset(*rd.file, token.span.start).column == 1;
}

bool ReadTokens(Reader& rd, const std::vector<Token>& tokens) {
    for (const Token& token : tokens) {
        if (rd.recover && rd.errors->size() >= kMaxReadErrors) {
            break;
        }
        if (token.kind == TokenKind::Comment) {
            rd.tree->comments.push_back(token.span);
            continue;
        }
        if (token.kind == TokenKind::OpenParen) {
            if (rd.recover && rd.open_parens > 0 && AtColumnOne(rd, token)) {
                // a top-level form starts here, so the form around it is missing a ')'
                rd.has_column_one_open = true;
                rd.column_one_open = token.span.start;
                ReadError error;
                FailUnclosed(rd, error);
                rd.errors->push_back(error);
                CloseAllOpen(rd);
            }
            OpenList(rd, token);
        } else if (token.kind == TokenKind::CloseParen) {
            ReadError error;
            if (!CloseList(rd, token, error)) {
                rd.errors->push_back(error);
                if (!rd.recover) {
                    return false;
                }
            }
        } else if (IsQuoteMark(token.kind)) {
            OpenMark(rd, token);
        } else {
            AddForm(rd, AtomFromToken(rd, token));
        }
        rd.last_end = token.span.end;
    }
    if (!rd.stack.empty()) {
        ReadError error;
        FailUnclosed(rd, error);
        rd.errors->push_back(error);
        if (!rd.recover) {
            return false;
        }
        CloseAllOpen(rd);
    }
    return true;
}

} // namespace

bool ReadTree(const SourceFile& file, Tree& out, ReadError& error) {
    out = Tree();
    std::vector<Token> tokens;
    if (!Lex(file, tokens, error)) {
        return false;
    }
    std::vector<ReadError> errors;
    Reader rd;
    rd.file = &file;
    rd.tree = &out;
    rd.errors = &errors;
    if (!ReadTokens(rd, tokens)) {
        error = errors[0];
        return false;
    }
    return true;
}

void ReadTreeRecovering(const SourceFile& file, Tree& out, std::vector<ReadError>& errors) {
    out = Tree();
    std::vector<Token> tokens;
    LexRecovering(file, tokens, errors);
    Reader rd;
    rd.file = &file;
    rd.tree = &out;
    rd.recover = true;
    rd.errors = &errors;
    ReadTokens(rd, tokens);
}

std::string FormatReadError(const SourceFile& file, const ReadError& error) {
    std::string out;
    auto add_marker = [&](std::uint32_t offset, const char* level, const std::string& message) {
        SourcePos pos = PosFromOffset(file, offset);
        out += file.path + ":" + std::to_string(pos.line) + ":" + std::to_string(pos.column) +
               ": " + level + ": " + message + "\n";
        std::uint32_t line_start = file.line_starts[pos.line - 1];
        std::uint32_t line_end = line_start;
        while (line_end < file.text.size() && file.text[line_end] != '\n') {
            line_end++;
        }
        std::string number = std::to_string(pos.line);
        out += " " + number + " | " + file.text.substr(line_start, line_end - line_start) + "\n";
        out += " " + std::string(number.size(), ' ') + " | ";
        // keep tabs so the caret lines up under tabbed source
        for (std::uint32_t i = line_start; i < offset; i++) {
            out += file.text[i] == '\t' ? '\t' : ' ';
        }
        out += "^\n";
    };
    add_marker(error.offset, "error", error.message);
    if (error.has_note) {
        add_marker(error.note_offset, "note", error.note);
    }
    return out;
}

bool TreesEqual(const Node& a, const Node& b) {
    if (a.kind != b.kind) {
        return false;
    }
    if (a.kind != NodeKind::List) {
        return a.text == b.text;
    }
    return FormsEqual(a.items, b.items);
}

bool FormsEqual(const std::vector<Node>& a, const std::vector<Node>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); i++) {
        if (!TreesEqual(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

bool ShowsMark(const Node& node) {
    if (node.mark == QuoteMark::None || node.items.size() != 2) {
        return false;
    }
    const Node& form = node.items[1];
    bool starts_with_char = form.kind == NodeKind::List && form.mark == QuoteMark::None &&
                            !form.items.empty() && form.items[0].kind == NodeKind::Integer &&
                            !form.items[0].text.empty() && form.items[0].text[0] == '\'';
    return node.mark != QuoteMark::Quote || !starts_with_char;
}

void PrintFlatNode(const Node& node, std::string& out) {
    if (node.kind != NodeKind::List) {
        out += node.text;
        return;
    }
    if (ShowsMark(node)) {
        out += QuoteMarkText(node.mark);
        PrintFlatNode(node.items[1], out);
        return;
    }
    out += '(';
    for (std::size_t i = 0; i < node.items.size(); i++) {
        if (i > 0) {
            out += ' ';
        }
        PrintFlatNode(node.items[i], out);
    }
    out += ')';
}

std::string PrintFlat(const std::vector<Node>& forms) {
    std::string out;
    for (std::size_t i = 0; i < forms.size(); i++) {
        if (i > 0) {
            out += ' ';
        }
        PrintFlatNode(forms[i], out);
    }
    return out;
}

const char* QuoteMarkText(QuoteMark mark) {
    switch (mark) {
    case QuoteMark::Quote:
        return "'";
    case QuoteMark::Quasiquote:
        return "`";
    case QuoteMark::Unquote:
        return ",";
    case QuoteMark::UnquoteSplicing:
        return ",@";
    default:
        return "";
    }
}

const char* QuoteMarkName(QuoteMark mark) {
    switch (mark) {
    case QuoteMark::Quote:
        return "quote";
    case QuoteMark::Quasiquote:
        return "quasiquote";
    case QuoteMark::Unquote:
        return "unquote";
    case QuoteMark::UnquoteSplicing:
        return "unquote_splicing";
    default:
        return "";
    }
}

} // namespace tisple
