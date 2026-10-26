#pragma once

#include "reader/lexer.hpp"
#include "source_file.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

enum class NodeKind : std::uint8_t {
    List,
    Symbol,
    Keyword, // :name, syntax only
    Integer,
    String,
};

// how a list was written: plain parentheses or a quote mark ('x is (quote x))
enum class QuoteMark : std::uint8_t {
    None,
    Quote,
    Quasiquote,
    Unquote,
    UnquoteSplicing,
};

// one form of the plain tree; layout lives only in spans and Tree::comments
struct Node {
    NodeKind kind = NodeKind::List;
    Span span;
    std::string text;       // atoms as written: symbol name, ":key", "0x7F", "\"hi\\n\""
    std::int64_t value = 0; // integers
    std::string bytes;      // strings, escapes decoded
    QuoteMark mark = QuoteMark::None;
    std::vector<Node> items;     // lists; a marked list is (quote_name form)
    std::uint32_t expansion = 0; // 0: written in the source; n: made by macro call n - 1
};

// a whole file: top-level forms plus every comment's span, in source order
struct Tree {
    std::vector<Node> forms;
    std::vector<Span> comments;
};

bool ReadTree(const SourceFile& file, Tree& out, ReadError& error);

// "path:line:col: error: ..." with the source line and a caret, plus the note
std::string FormatReadError(const SourceFile& file, const ReadError& error);

// same structure and atom spellings; spans, comments and marks ignored
bool TreesEqual(const Node& a, const Node& b);
bool FormsEqual(const std::vector<Node>& a, const std::vector<Node>& b);

// every form on one line, single spaces, no comments: the "mashed" program
std::string PrintFlat(const std::vector<Node>& forms);
void PrintFlatNode(const Node& node, std::string& out);

const char* QuoteMarkText(QuoteMark mark);
const char* QuoteMarkName(QuoteMark mark);

} // namespace tisple
