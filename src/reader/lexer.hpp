#pragma once

#include "source_file.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

// byte range [start, end) in the source text
struct Span {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
};

enum class TokenKind : std::uint8_t {
    OpenParen,
    CloseParen,
    Symbol,
    Keyword, // :name
    Integer,
    String,
    Comment,
    Quote,           // '
    Quasiquote,      // `
    Unquote,         // ,
    UnquoteSplicing, // ,@
};

struct Token {
    TokenKind kind = TokenKind::Symbol;
    Span span;
    std::int64_t value = 0; // integers only
};

// first problem found while reading; the note points somewhere related
struct ReadError {
    std::uint32_t offset = 0;
    std::string message;
    bool has_note = false;
    std::uint32_t note_offset = 0;
    std::string note;
};

// largest magnitude an integer literal may have; the checker narrows by type
constexpr std::int64_t kMaxIntegerLiteral = 0xFFFFFFFF;

// tokens before the first error stay in out, so the viewer can still color them
bool Lex(const SourceFile& file, std::vector<Token>& out, ReadError& error);

bool IsSpaceChar(char c);
bool IsDelimiterChar(char c);
bool IsQuoteMark(TokenKind kind);

} // namespace tisple
