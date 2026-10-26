#include "reader/lexer.hpp"

namespace tisple {

namespace {

struct Lexer {
    const std::string* text = nullptr;
    std::uint32_t pos = 0;
    std::vector<Token>* out = nullptr;
    ReadError* error = nullptr;
};

bool Fail(Lexer& lx, std::uint32_t offset, const std::string& message) {
    lx.error->offset = offset;
    lx.error->message = message;
    return false;
}

char At(const Lexer& lx, std::uint32_t offset) {
    if (offset >= lx.text->size()) {
        return '\0';
    }
    return (*lx.text)[offset];
}

bool AtEnd(const Lexer& lx, std::uint32_t offset) {
    return offset >= lx.text->size();
}

void Push(Lexer& lx, TokenKind kind, std::uint32_t start, std::uint32_t end) {
    Token token;
    token.kind = kind;
    token.span.start = start;
    token.span.end = end;
    lx.out->push_back(token);
}

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

// end of the word starting at start: everything up to a delimiter
std::uint32_t WordEnd(const Lexer& lx, std::uint32_t start) {
    std::uint32_t end = start;
    while (!AtEnd(lx, end) && !IsDelimiterChar(At(lx, end))) {
        end++;
    }
    return end;
}

int DigitValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return 99;
}

bool LexInteger(Lexer& lx) {
    std::uint32_t start = lx.pos;
    std::uint32_t end = WordEnd(lx, start);
    std::uint32_t p = start;
    bool negative = false;
    if (At(lx, p) == '-') {
        negative = true;
        p++;
    }

    // base prefix
    int base = 10;
    const char* base_name = "decimal";
    char second = At(lx, p + 1);
    if (At(lx, p) == '0' && (second == 'x' || second == 'X')) {
        base = 16;
        base_name = "hex";
        p += 2;
    } else if (At(lx, p) == '0' && (second == 'b' || second == 'B')) {
        base = 2;
        base_name = "binary";
        p += 2;
    }
    if (p >= end) {
        return Fail(lx, start, std::string(base_name) + " integer has no digits");
    }

    std::int64_t value = 0;
    for (; p < end; p++) {
        int digit = DigitValue(At(lx, p));
        if (digit >= base) {
            return Fail(lx, p,
                        std::string("bad digit '") + At(lx, p) + "' in " + base_name + " integer");
        }
        value = value * base + digit;
        if (value > kMaxIntegerLiteral) {
            return Fail(lx, start, "integer is too large");
        }
    }

    Push(lx, TokenKind::Integer, start, end);
    lx.out->back().value = negative ? -value : value;
    lx.pos = end;
    return true;
}

// checks the characters of a symbol or keyword name
bool CheckName(Lexer& lx, std::uint32_t name_start, std::uint32_t end) {
    for (std::uint32_t p = name_start; p < end; p++) {
        char c = At(lx, p);
        if (c == ':') {
            return Fail(lx, p, "':' may only start a keyword");
        }
        if (c == '#' && p + 1 != end) {
            return Fail(lx, p, "'#' may only end a name");
        }
        if (c == '#' && p == name_start) {
            return Fail(lx, p, "'#' must follow a name");
        }
        auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            return Fail(lx, p, "unexpected control character");
        }
    }
    return true;
}

bool LexSymbol(Lexer& lx) {
    std::uint32_t start = lx.pos;
    std::uint32_t end = WordEnd(lx, start);
    bool keyword = At(lx, start) == ':';
    if (keyword && end == start + 1) {
        return Fail(lx, start, "keyword needs a name after ':'");
    }
    if (!CheckName(lx, keyword ? start + 1 : start, end)) {
        return false;
    }
    Push(lx, keyword ? TokenKind::Keyword : TokenKind::Symbol, start, end);
    lx.pos = end;
    return true;
}

bool IsHexDigit(char c) {
    return DigitValue(c) < 16;
}

bool LexString(Lexer& lx) {
    std::uint32_t start = lx.pos;
    std::uint32_t p = start + 1;
    while (true) {
        if (AtEnd(lx, p) || At(lx, p) == '\n') {
            return Fail(lx, start, "string is not closed on this line");
        }
        char c = At(lx, p);
        if (c == '"') {
            break;
        }
        if (c != '\\') {
            p++;
            continue;
        }

        // escapes: \n \t \r \0 \\ \" \xHH
        char e = At(lx, p + 1);
        if (e == 'n' || e == 't' || e == 'r' || e == '0' || e == '\\' || e == '"') {
            p += 2;
            continue;
        }
        if (e == 'x' && IsHexDigit(At(lx, p + 2)) && IsHexDigit(At(lx, p + 3))) {
            p += 4;
            continue;
        }
        if (e == 'x') {
            return Fail(lx, p, "\\x needs two hex digits");
        }
        return Fail(lx, p, std::string("unknown escape '\\") + e + "'");
    }
    Push(lx, TokenKind::String, start, p + 1);
    lx.pos = p + 1;
    return true;
}

bool LexQuoteMark(Lexer& lx) {
    std::uint32_t start = lx.pos;
    char c = At(lx, start);
    TokenKind kind = TokenKind::Quote;
    std::uint32_t end = start + 1;
    if (c == '`') {
        kind = TokenKind::Quasiquote;
    } else if (c == ',' && At(lx, start + 1) == '@') {
        kind = TokenKind::UnquoteSplicing;
        end = start + 2;
    } else if (c == ',') {
        kind = TokenKind::Unquote;
    }

    // the form must follow directly so the formatter can keep them together
    char next = At(lx, end);
    if (AtEnd(lx, end) || IsSpaceChar(next) || next == ')' || next == ';') {
        std::string mark = lx.text->substr(start, end - start);
        return Fail(lx, start, "'" + mark + "' must be followed directly by a form");
    }
    Push(lx, kind, start, end);
    lx.pos = end;
    return true;
}

bool LexOne(Lexer& lx) {
    char c = At(lx, lx.pos);
    std::uint32_t start = lx.pos;
    if (c == '(' || c == ')') {
        Push(lx, c == '(' ? TokenKind::OpenParen : TokenKind::CloseParen, start, start + 1);
        lx.pos++;
        return true;
    }
    if (c == ';') {
        std::uint32_t end = start;
        while (!AtEnd(lx, end) && At(lx, end) != '\n') {
            end++;
        }
        Push(lx, TokenKind::Comment, start, end);
        lx.pos = end;
        return true;
    }
    if (c == '"') {
        return LexString(lx);
    }
    if (c == '\'' || c == '`' || c == ',') {
        return LexQuoteMark(lx);
    }
    if (IsDigit(c) || (c == '-' && IsDigit(At(lx, start + 1)))) {
        return LexInteger(lx);
    }
    return LexSymbol(lx);
}

// recovery: a bad token becomes spaces so the lexer can go on past it. A string runs to the
// end of its line; anything else is the word around the problem, or one character
void BlankBadToken(std::string& text, std::uint32_t offset) {
    if (offset >= text.size()) {
        return;
    }
    std::uint32_t start = offset;
    std::uint32_t end = offset;
    if (text[offset] == '"') {
        while (end < text.size() && text[end] != '\n') {
            end++;
        }
    } else {
        while (start > 0 && !IsDelimiterChar(text[start - 1])) {
            start--;
        }
        while (end < text.size() && !IsDelimiterChar(text[end])) {
            end++;
        }
    }
    if (end == start) {
        end = start + 1;
    }
    for (std::uint32_t i = start; i < end; i++) {
        text[i] = text[i] == '\n' ? '\n' : ' ';
    }
}

} // namespace

bool IsSpaceChar(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool IsDelimiterChar(char c) {
    return IsSpaceChar(c) || c == '(' || c == ')' || c == ';' || c == '"' || c == '\'' ||
           c == '`' || c == ',';
}

bool IsQuoteMark(TokenKind kind) {
    return kind == TokenKind::Quote || kind == TokenKind::Quasiquote ||
           kind == TokenKind::Unquote || kind == TokenKind::UnquoteSplicing;
}

bool Lex(const SourceFile& file, std::vector<Token>& out, ReadError& error) {
    Lexer lx;
    lx.text = &file.text;
    lx.out = &out;
    lx.error = &error;
    while (!AtEnd(lx, lx.pos)) {
        if (IsSpaceChar(At(lx, lx.pos))) {
            lx.pos++;
            continue;
        }
        if (!LexOne(lx)) {
            return false;
        }
    }
    return true;
}

void LexRecovering(const SourceFile& file, std::vector<Token>& tokens,
                   std::vector<ReadError>& errors) {
    SourceFile work = file;
    while (true) {
        tokens.clear();
        ReadError error;
        if (Lex(work, tokens, error)) {
            return;
        }
        errors.push_back(error);
        if (errors.size() >= kMaxReadErrors) {
            return;
        }
        BlankBadToken(work.text, error.offset);
    }
}

} // namespace tisple
