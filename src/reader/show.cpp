#include "reader/show.hpp"

#include "reader/keywords.hpp"
#include "reader/reader.hpp"

namespace tisple {

namespace {

// 256-color scheme for dark terminals
const char* const kReset = "\x1b[0m";
const char* const kGutter = "\x1b[38;5;239m";
const char* const kSpecial = "\x1b[1;38;5;176m";
const char* const kType = "\x1b[38;5;80m";
const char* const kBuiltin = "\x1b[38;5;111m";
const char* const kConstant = "\x1b[38;5;222m";
const char* const kNumber = "\x1b[38;5;215m";
const char* const kString = "\x1b[38;5;114m";
const char* const kComment = "\x1b[3;38;5;245m";
const char* const kKeyword = "\x1b[38;5;217m";
const char* const kQuote = "\x1b[1;38;5;203m";
const char* const kErrorToken = "\x1b[1;97;41m";
const char* const kErrorText = "\x1b[1;38;5;203m";
const char* const kNoteText = "\x1b[1;38;5;221m";
const char* const kDim = "\x1b[38;5;240m";
const char* const kParens[] = {
    "\x1b[38;5;179m", "\x1b[38;5;140m", "\x1b[38;5;73m",
    "\x1b[38;5;173m", "\x1b[38;5;108m", "\x1b[38;5;175m",
};
constexpr int kParenColors = 6;

struct Marker {
    bool active = false;
    std::uint32_t offset = 0;
    std::string message;
    bool is_note = false;
};

struct Viewer {
    const SourceFile* file = nullptr;
    bool color = false;
    std::string* out = nullptr;
    std::vector<Token> tokens;
    std::size_t next_token = 0;
    int depth = 0;
    std::uint32_t color_limit = 0; // bytes from here on print dim
    bool lex_failed = false;
    Marker error;
    Marker note;
};

void Styled(Viewer& vw, const char* style, const std::string& text) {
    if (vw.color && style) {
        *vw.out += style;
        *vw.out += text;
        *vw.out += kReset;
        return;
    }
    *vw.out += text;
}

const char* SymbolStyle(const std::string& text) {
    switch (ClassifySymbol(text)) {
    case SymbolClass::SpecialForm:
        return kSpecial;
    case SymbolClass::Type:
        return kType;
    case SymbolClass::Builtin:
        return kBuiltin;
    case SymbolClass::Constant:
        return kConstant;
    default:
        return nullptr;
    }
}

const char* TokenStyle(Viewer& vw, const Token& token, const std::string& text) {
    switch (token.kind) {
    case TokenKind::OpenParen:
        return kParens[vw.depth++ % kParenColors];
    case TokenKind::CloseParen:
        if (vw.depth == 0) {
            return kErrorToken;
        }
        return kParens[--vw.depth % kParenColors];
    case TokenKind::Symbol:
        return SymbolStyle(text);
    case TokenKind::Keyword:
        return kKeyword;
    case TokenKind::Integer:
        return kNumber;
    case TokenKind::String:
        return kString;
    case TokenKind::Comment:
        return kComment;
    default:
        return kQuote;
    }
}

bool MarkedAt(const Viewer& vw, std::uint32_t offset) {
    return (vw.error.active && vw.error.offset == offset) ||
           (vw.note.active && vw.note.offset == offset);
}

std::uint32_t LineEnd(const SourceFile& file, std::uint32_t start) {
    std::uint32_t end = start;
    while (end < file.text.size() && file.text[end] != '\n') {
        end++;
    }
    return end;
}

void RenderLineText(Viewer& vw, std::uint32_t start, std::uint32_t end) {
    const std::string& text = vw.file->text;
    std::uint32_t p = start;
    while (p < end) {
        // skip tokens that were consumed earlier
        while (vw.next_token < vw.tokens.size() && vw.tokens[vw.next_token].span.start < p) {
            vw.next_token++;
        }
        bool token_here = vw.next_token < vw.tokens.size() &&
                          vw.tokens[vw.next_token].span.start == p && p < vw.color_limit;
        if (token_here) {
            const Token& token = vw.tokens[vw.next_token];
            std::string piece = text.substr(p, token.span.end - p);
            const char* style = TokenStyle(vw, token, piece);
            Styled(vw, MarkedAt(vw, p) ? kErrorToken : style, piece);
            p = token.span.end;
            vw.next_token++;
            continue;
        }
        if (vw.lex_failed && p == vw.error.offset) {
            // the lexer stopped here: mark the bad character
            Styled(vw, kErrorToken, text.substr(p, 1));
            p++;
            continue;
        }
        if (p >= vw.color_limit) {
            Styled(vw, kDim, text.substr(p, end - p));
            break;
        }
        *vw.out += text[p];
        p++;
    }
}

void RenderMarker(Viewer& vw, const Marker& marker, std::uint32_t gutter_width) {
    SourcePos pos = PosFromOffset(*vw.file, marker.offset);
    std::uint32_t line_start = vw.file->line_starts[pos.line - 1];
    std::string pad(gutter_width + 2, ' ');
    for (std::uint32_t i = line_start; i < marker.offset; i++) {
        pad += vw.file->text[i] == '\t' ? '\t' : ' ';
    }
    *vw.out += pad;
    std::string label = marker.is_note ? "note: " : "error: ";
    Styled(vw, marker.is_note ? kNoteText : kErrorText, "^ " + label + marker.message);
    *vw.out += '\n';
}

void RenderLines(Viewer& vw) {
    const SourceFile& file = *vw.file;
    std::uint32_t line_count = static_cast<std::uint32_t>(file.line_starts.size());
    if (line_count > 1 && file.line_starts.back() == file.text.size()) {
        line_count--; // the empty line after a final newline
    }
    std::uint32_t gutter_width = static_cast<std::uint32_t>(std::to_string(line_count).size());
    for (std::uint32_t line = 0; line < line_count; line++) {
        std::string number = std::to_string(line + 1);
        std::string gutter = std::string(gutter_width - number.size(), ' ') + number;
        Styled(vw, kGutter, gutter);
        std::uint32_t start = file.line_starts[line];
        std::uint32_t end = LineEnd(file, start);
        if (end > start) {
            *vw.out += "  ";
        }
        RenderLineText(vw, start, end);
        *vw.out += '\n';
        for (const Marker* marker : {&vw.error, &vw.note}) {
            if (marker->active && PosFromOffset(file, marker->offset).line == line + 1) {
                RenderMarker(vw, *marker, gutter_width);
            }
        }
    }
}

} // namespace

bool RenderShow(const SourceFile& file, bool color, std::string& out) {
    Viewer vw;
    vw.file = &file;
    vw.color = color;
    vw.out = &out;
    vw.color_limit = static_cast<std::uint32_t>(file.text.size());

    ReadError error;
    vw.lex_failed = !Lex(file, vw.tokens, error);
    bool ok = !vw.lex_failed;
    if (ok) {
        Tree tree;
        ok = ReadTree(file, tree, error);
    }
    if (!ok) {
        // color up to the error (a lex error) or its line's end (a paren error), dim after
        vw.error = Marker{true, error.offset, error.message, false};
        if (error.has_note) {
            vw.note = Marker{true, error.note_offset, error.note, true};
        }
        vw.color_limit = vw.lex_failed ? error.offset : LineEnd(file, error.offset);
    }
    RenderLines(vw);
    return ok;
}

} // namespace tisple
