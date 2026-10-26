#include "test_reader.hpp"

#include "reader/reader.hpp"

#include <algorithm>
#include <filesystem>

namespace tisple_tests {

namespace {

using tisple::NodeKind;
using tisple::QuoteMark;
using tisple::ReadError;
using tisple::Tree;

bool Reads(const std::string& text, Tree& tree) {
    ReadError error;
    return tisple::ReadTree(Text(text), tree, error);
}

// reads must fail at line:column with a message containing what
bool FailsAt(const std::string& text, std::uint32_t line, std::uint32_t column,
             const std::string& what) {
    tisple::SourceFile file = Text(text);
    Tree tree;
    ReadError error;
    if (tisple::ReadTree(file, tree, error)) {
        return false;
    }
    tisple::SourcePos pos = tisple::PosFromOffset(file, error.offset);
    bool ok =
        pos.line == line && pos.column == column && error.message.find(what) != std::string::npos;
    if (!ok) {
        std::fprintf(stderr, "  got %u:%u %s\n", pos.line, pos.column, error.message.c_str());
    }
    return ok;
}

void TestAtoms(TestResults& results) {
    Tree tree;
    CHECK(results,
          Reads("(fn f 12 0x7F 0b101 -5 \"a\\n\\x41\" :align <= . hgr.plot gfx/hgr)", tree));
    CHECK(results, tree.forms.size() == 1);
    const auto& items = tree.forms[0].items;
    CHECK(results, items.size() == 12);
    CHECK(results, items[0].kind == NodeKind::Symbol && items[0].text == "fn");
    CHECK(results, items[2].kind == NodeKind::Integer && items[2].value == 12);
    CHECK(results, items[3].value == 0x7F && items[3].text == "0x7F");
    CHECK(results, items[4].value == 5);
    CHECK(results, items[5].value == -5);
    CHECK(results, items[6].kind == NodeKind::String && items[6].bytes == "a\nA");
    CHECK(results, items[7].kind == NodeKind::Keyword && items[7].text == ":align");
    CHECK(results, items[8].kind == NodeKind::Symbol && items[8].text == "<=");
    CHECK(results, items[9].kind == NodeKind::Symbol && items[9].text == ".");
    CHECK(results, items[10].text == "hgr.plot" && items[11].text == "gfx/hgr");
    CHECK(results, tree.forms[0].span.start == 0);

    // tmp# and a lone '-' are symbols
    CHECK(results, Reads("(let tmp# (- 0 1))", tree));
    CHECK(results, tree.forms[0].items[1].text == "tmp#");
    CHECK(results, tree.forms[0].items[2].items[0].kind == NodeKind::Symbol);
}

void TestStructure(TestResults& results) {
    Tree tree;
    // whitespace means nothing: mashed and spread out read the same
    Tree spread;
    CHECK(results, Reads("(fn f (args (x u8))\n    ; hi\n    (+ x 1))", spread));
    CHECK(results, Reads("(fn f(args(x u8))(+ x 1))", tree));
    CHECK(results, tisple::FormsEqual(tree.forms, spread.forms));
    CHECK(results, spread.comments.size() == 1);
    CHECK(results, tree.comments.empty());

    // spans cover the source text
    CHECK(results, Reads("  (a (b c))", tree));
    CHECK(results, tree.forms[0].span.start == 2 && tree.forms[0].span.end == 11);
    CHECK(results, tree.forms[0].items[1].span.start == 5);

    CHECK(results, Reads("", tree) && tree.forms.empty());
    CHECK(results, Reads("; only a comment", tree) && tree.comments.size() == 1);
    CHECK(results, Reads("()", tree) && tree.forms[0].items.empty());
}

void TestQuoteMarks(TestResults& results) {
    Tree tree;
    CHECK(results, Reads("`(do (let tmp# ,a) ,@body 'x)", tree));
    const tisple::Node& qq = tree.forms[0];
    CHECK(results, qq.mark == QuoteMark::Quasiquote && qq.items.size() == 2);
    CHECK(results, qq.items[0].text == "quasiquote");
    const tisple::Node& body = qq.items[1];
    CHECK(results, body.items[1].items[2].mark == QuoteMark::Unquote);
    CHECK(results, body.items[1].items[2].items[0].text == "unquote");
    CHECK(results, body.items[2].mark == QuoteMark::UnquoteSplicing);
    CHECK(results, body.items[2].items[0].text == "unquote_splicing");
    CHECK(results, body.items[3].mark == QuoteMark::Quote);

    // the mark is spelling only: 'x and (quote x) are the same tree
    Tree long_form;
    CHECK(results, Reads("(quote x)", long_form) && Reads("'x", tree));
    CHECK(results, tisple::FormsEqual(tree.forms, long_form.forms));
    CHECK(results, tisple::PrintFlat(tree.forms) == "'x");
    CHECK(results, tisple::PrintFlat(long_form.forms) == "(quote x)");
    CHECK(results, Reads("''x", tree) && tisple::PrintFlat(tree.forms) == "''x");
}

void TestLexErrors(TestResults& results) {
    CHECK(results, FailsAt("(let x 0x)", 1, 8, "hex integer has no digits"));
    CHECK(results, FailsAt("(let x 0x1G)", 1, 11, "bad digit 'G' in hex"));
    CHECK(results, FailsAt("(let x 0b102)", 1, 12, "bad digit '2' in binary"));
    CHECK(results, FailsAt("\n  (f 12ab)", 2, 8, "bad digit 'a' in decimal"));
    CHECK(results, FailsAt("(f 99999999999)", 1, 4, "too large"));
    CHECK(results, FailsAt("(f \"abc)\n", 1, 4, "string is not closed"));
    CHECK(results, FailsAt("(f \"a\\qb\")", 1, 6, "unknown escape"));
    CHECK(results, FailsAt("(f \"\\x4\")", 1, 5, "two hex digits"));
    CHECK(results, FailsAt("(f a#b)", 1, 5, "'#' may only end a name"));
    CHECK(results, FailsAt("(f #)", 1, 4, "'#' must follow a name"));
    CHECK(results, FailsAt("(f a:b)", 1, 5, "':' may only start a keyword"));
    CHECK(results, FailsAt("(f :)", 1, 4, "keyword needs a name"));
    CHECK(results, FailsAt("(f ' x)", 1, 4, "must be followed directly"));
    CHECK(results, FailsAt("(f ,@)", 1, 4, "must be followed directly"));
}

void TestParenErrors(TestResults& results) {
    // the '(' that never closed, not the end of the file
    CHECK(results, FailsAt("(fn a\n    (let x 1)\n    (+ x 1)\n", 1, 1, "never closed"));
    CHECK(results, FailsAt("(a)\n(b (c)\n(d)", 2, 1, "never closed"));
    CHECK(results, FailsAt("(a))", 1, 4, "no matching '('"));
    CHECK(results, FailsAt(")", 1, 1, "no matching '('"));

    // a form at column 1 inside an open one hints where the ')' went missing
    tisple::SourceFile file = Text("(fn a\n    (let x (f 1)\n    x)\n(fn b\n    2)\n");
    Tree tree;
    ReadError error;
    CHECK(results, !tisple::ReadTree(file, tree, error));
    CHECK(results, error.has_note && tisple::PosFromOffset(file, error.note_offset).line == 4);
    std::string text = tisple::FormatReadError(file, error);
    CHECK(results, text.find("t.tsp:1:1: error: this '(' is never closed") == 0);
    CHECK(results, text.find("t.tsp:4:1: note:") != std::string::npos);

    // an extra ')' points back at where the form before it closed
    file = Text("(fn a\n    (f 1))\n    (g 2))\n");
    CHECK(results, !tisple::ReadTree(file, tree, error));
    CHECK(results, tisple::PosFromOffset(file, error.offset).line == 3);
    CHECK(results, error.has_note);
    tisple::SourcePos note = tisple::PosFromOffset(file, error.note_offset);
    CHECK(results, note.line == 2 && note.column == 10);
}

} // namespace

tisple::SourceFile Text(const std::string& text) {
    return tisple::SourceFileFromText("t.tsp", text);
}

std::string SourcePath(const std::string& relative) {
    return std::string(TISPLE_SOURCE_DIR) + "/" + relative;
}

std::vector<std::string> ExampleFiles() {
    std::vector<std::string> paths;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(SourcePath("examples"))) {
        if (entry.is_regular_file() && entry.path().extension() == ".tsp") {
            paths.push_back(entry.path().string());
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

void RunReaderTests(TestResults& results) {
    TestAtoms(results);
    TestStructure(results);
    TestQuoteMarks(results);
    TestLexErrors(results);
    TestParenErrors(results);
}

} // namespace tisple_tests
