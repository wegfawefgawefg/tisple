#pragma once

// shared by format_notes.cpp (what the source says) and format.cpp (layout)

#include "reader/format.hpp"

#include <unordered_map>
#include <vector>

namespace tisple {

// layout facts about one node, gathered before printing
struct NodeNotes {
    const Node* parent = nullptr;          // null at top level
    std::uint32_t index = 0;               // position in parent's items or in the forms
    std::vector<std::uint32_t> leading;    // own-line comments just before the node
    std::vector<std::uint32_t> after_open; // comments right after '(' (moved to line end)
    std::vector<std::uint32_t> trailing;   // comments after the node's last token
    std::vector<std::uint32_t> dangling;   // own-line comments before ')'
    bool break_before = false;             // a trailing comment ended the line before this node
    bool blank_before = false;             // blank line right before the node itself
    bool flat_ok = true;                   // may print on one line
    std::uint32_t flat_width = 0;
};

// a trailing comment in the output, for lining up runs of them
struct TrailingMark {
    std::uint32_t line = 0;
    std::uint32_t column = 0; // where the code before it ends
};

struct Formatter {
    const SourceFile* file = nullptr;
    const Tree* tree = nullptr;
    bool keep_trivia = true;
    std::unordered_map<const Node*, NodeNotes> notes;
    std::unordered_map<std::uint32_t, const Node*> by_start;
    std::unordered_map<std::uint32_t, const Node*> by_end;
    std::vector<bool> comment_blank_before;
    std::vector<std::uint32_t> top_dangling; // comments after the last form

    // output
    std::string out;
    std::uint32_t column = 0;
    std::uint32_t line = 0;
    std::vector<std::uint32_t> pending; // trailing comments waiting for the end of the line
    std::vector<TrailingMark> marks;
};

void BuildNotes(Formatter& fm);

// -1 when not a body form; otherwise how many items after the head stay on the head line
int BodyHeaderCount(const Node& node);
bool HasBody(const Node& node);
// args, enum, struct: one line if it fits, else one item per line; returns items kept
int OnePerLineKeep(const Node& node);

} // namespace tisple
