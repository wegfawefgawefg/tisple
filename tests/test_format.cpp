#include "reader/format.hpp"
#include "test_reader.hpp"

#include <algorithm>
#include <cstdlib>
#include <random>

namespace tisple_tests {

namespace {

using tisple::ReadError;
using tisple::Tree;

std::string Format(const std::string& text) {
    std::string out;
    ReadError error;
    if (!tisple::FormatSource(Text(text), out, error)) {
        return "<read error: " + error.message + ">";
    }
    return out;
}

std::string Mash(const std::string& text) {
    Tree tree;
    ReadError error;
    if (!tisple::ReadTree(Text(text), tree, error)) {
        return "<read error>";
    }
    return tisple::PrintFlat(tree.forms);
}

// text is already canonical, and its one-line mashed form formats back to it
bool Canonical(const std::string& text) {
    std::string formatted = Format(text);
    std::string from_mash = Format(Mash(text));
    if (formatted != text || from_mash != text) {
        std::fprintf(stderr, "  want:\n%s  got:\n%s  from mashed:\n%s", text.c_str(),
                     formatted.c_str(), from_mash.c_str());
        return false;
    }
    return true;
}

// formats to want (want ends in a newline)
bool FormatsTo(const std::string& text, const std::string& want) {
    std::string got = Format(text);
    if (got != want) {
        std::fprintf(stderr, "  want:\n%s  got:\n%s", want.c_str(), got.c_str());
        return false;
    }
    return Format(got) == got;
}

void TestBodyForms(TestResults& results) {
    // name, parameters and return type stay on the head line
    CHECK(results, Canonical("(fn fill ((dst (ptr u8)) (count u16) (value u8)) void\n"
                             "    (for i u16 0 count\n"
                             "        (store (+ dst i) value)))\n"));
    CHECK(results, Canonical("(fn read_key () u8\n"
                             "    (let key u8 (io_read u8 0xC000))\n"
                             "    (bit_and key 0x7F))\n"));
    CHECK(results, Canonical("(fn f () void)\n"));
    // if arms always get their own lines, even when short
    CHECK(results, FormatsTo("(fn f () void (if c (a) (b)))",
                             "(fn f () void\n    (if c\n        (a)\n        (b)))\n"));
    CHECK(results, Canonical("(while (not (ready)))\n"));
    CHECK(results, Canonical("(do)\n"));
    // a clause of one form that fits stays on one line; otherwise its body goes below
    CHECK(results, Canonical("(match a\n"
                             "    (Action.left (step 255))\n"
                             "    (Action.right\n"
                             "        (if (< x 10)\n"
                             "            (step 1)))\n"
                             "    (Action.fire\n"
                             "        (shoot)\n"
                             "        (click))\n"
                             "    (_))\n"));
    // a body form as the last argument hangs off the head line
    CHECK(results, Canonical("(let m u8 (if (< a b)\n    a\n    b))\n"));
    CHECK(results, Canonical("(macro swap (a b)\n"
                             "    `(do\n"
                             "        (let tmp# u8 ,a)\n"
                             "        (set ,a ,b)\n"
                             "        (set ,b tmp#)))\n"));
}

void TestBreaking(TestResults& results) {
    std::string name(40, 'a');
    // general form: head and first argument stay, the rest go one per line at +4
    CHECK(results, Canonical("(fn f () void\n"
                             "    (store (+ " +
                             name +
                             " 1)\n"
                             "        (bit_or (load " +
                             name + ") (shl 1 bit))))\n"));
    // a first argument that does not fit goes down too
    CHECK(results, Canonical("(" + name + "\n    (" + name + " " + name + ")\n    x)\n"));
    // atoms fill lines
    std::string atoms;
    for (int i = 0; i < 40; i++) {
        atoms += " 0x" + std::to_string(1000 + i);
    }
    std::string run = Format("(f (array u16 40)" + atoms + ")");
    CHECK(results, run.find("(f (array u16 40)\n    0x1000 0x1001") == 0);
    CHECK(results, Format(Mash(run)) == run);
    // a long parameter list keeps its first parameter, then one per line at +8
    std::string long_params = "(fn draw ((dst (ptr u8)) (src (ptr u8)) (" + name +
                              " u8) (count u8) (step u8)) void (copy))";
    CHECK(results, FormatsTo(long_params, "(fn draw ((dst (ptr u8))\n"
                                          "        (src (ptr u8))\n"
                                          "        (" +
                                              name +
                                              " u8)\n"
                                              "        (count u8)\n"
                                              "        (step u8)) void\n"
                                              "    (copy))\n"));
    // enum and struct lists: like parameter lists
    CHECK(results, Canonical("(enum Action (none left right fire))\n"));
    CHECK(results, Canonical("(enum Key ((esc 0x1B) (space 0x20)))\n"));
    CHECK(results, Canonical("(struct Sprite ((x u8) (y u8)))\n"));
    CHECK(results,
          Canonical("(enum Big (" + name + "\n        " + name + "_b\n        " + name + "_c))\n"));
    CHECK(results, Canonical("(struct Big ((" + name + " u8)\n        (" + name +
                             "_b u16)\n        (" + name + "_c bool)))\n"));
    // a keyword and its value stay together; a declaration's type stays on the head line
    CHECK(results, Canonical("(let " + name + " (array u8 200)\n    :in " + name +
                             "\n    :align 256\n    :boundary 256)\n"));
    CHECK(results, FormatsTo("(const " + name + "\n (array u8 2)\n (make (array u8 2) 1 2))",
                             "(const " + name + " (array u8 2) (make (array u8 2) 1 2))\n"));
    CHECK(results, FormatsTo("(let " + name + "\n u8\n (+ " + name + " " + name + "))",
                             "(let " + name + " u8\n    (+ " + name + " " + name + "))\n"));
    // closing parens count toward the 100 columns
    std::string fits(92, 'x');
    CHECK(results, Canonical("(a (b " + fits + "))\n"));
    CHECK(results, Format("(a (b " + fits + "x))").find('\n') < 20);
}

// a run of atoms keeps the author's line breaks: a data table's rows mean something
void TestDataLists(TestResults& results) {
    std::string table = "(const ROW (array u16 12)\n"
                        "    (make (array u16 12)\n"
                        "        0x0400 0x0480 0x0500 0x0580\n"
                        "        0x0600 0x0680 0x0700 0x0780\n"
                        "\n"
                        "        0x0428 0x04A8 0x0528 0x05A8)\n"
                        "    :boundary 256)\n";
    CHECK(results, FormatsTo(table, table));
    // with no breaks to keep, as from a mashed line, the values fill lines to 100 columns
    CHECK(results, FormatsTo(Mash(table), "(const ROW (array u16 12)\n"
                                          "    (make (array u16 12)\n"
                                          "        0x0400 0x0480 0x0500 0x0580 0x0600 0x0680 "
                                          "0x0700 0x0780 0x0428 0x04A8 0x0528 0x05A8)\n"
                                          "    :boundary 256)\n"));
    // a row too long for one line fills on from where it breaks
    std::string atoms;
    for (int i = 0; i < 30; i++) {
        atoms += " 0x" + std::to_string(1000 + i);
    }
    std::string long_row = Format("(f g\n 1 2\n" + atoms + ")");
    CHECK(results, long_row.find("(f g\n    1 2\n    0x1000 0x1001") == 0);
    CHECK(results, long_row.find("0x1012\n    0x1013 0x1014") != std::string::npos);
    CHECK(results, std::count(long_row.begin(), long_row.end(), '\n') == 5);
    // a list that fits on one line is one line: only long runs keep their breaks
    CHECK(results, FormatsTo("(f 1\n 2\n 3)", "(f 1 2 3)\n"));
}

void TestComments(TestResults& results) {
    // trailing comments stay on their line and force the break after them
    CHECK(results, FormatsTo("(enum Command (none left ; arrow\n fire))",
                             "(enum Command (none\n        left ; arrow\n        fire))\n"));
    // trailing comments on consecutive lines line up
    CHECK(results, FormatsTo("(fn show () void\n (a) ; one\n (bbbb) ; two\n (c))\n",
                             "(fn show () void\n    (a)    ; one\n    (bbbb) ; two\n    (c))\n"));
    // a comment after the last item moves past the closing parens
    CHECK(results, FormatsTo("(fn f () void\n (a) ; done\n)", "(fn f () void\n    (a)) ; done\n"));
    CHECK(results, FormatsTo("(a b ; c\n)", "(a b) ; c\n"));
    // own-line comments keep their place and indent
    CHECK(results, FormatsTo("(fn f () void\n; first\n  (a)\n      ; second\n (b))",
                             "(fn f () void\n    ; first\n    (a)\n    ; second\n    (b))\n"));
    // a comment before ')' stays inside, with ')' on its own line
    CHECK(results,
          FormatsTo("(fn f () void\n (a)\n ; todo\n )", "(fn f () void\n    (a)\n    ; todo\n)\n"));
    // a comment right after '(' goes to the end of the head line
    CHECK(results,
          FormatsTo("(fn ; c\n f () void\n (a))", "(fn ; c\n        f () void\n    (a))\n"));
    CHECK(results, FormatsTo("( ; c\nfn f () void (a))", "(fn f () void ; c\n    (a))\n"));
    // file-level comments
    CHECK(results, FormatsTo("; top\n\n\n\n(a)\n; end", "; top\n\n(a)\n; end\n"));
    CHECK(results, FormatsTo("(a) ; x\n(b) ; y\n", "(a) ; x\n(b) ; y\n"));
}

void TestBlankLines(TestResults& results) {
    // single blank lines are kept, runs collapse, none at the start of a body
    CHECK(results, FormatsTo("(fn f () void\n\n (a)\n\n\n (b)\n (c))",
                             "(fn f () void\n    (a)\n\n    (b)\n    (c))\n"));
    // multi-line top-level forms always get a blank line around them
    CHECK(results,
          FormatsTo("(const A u8 1)\n(const B u8 2)\n(fn f () void (a))\n(fn g () void (b))",
                    "(const A u8 1)\n(const B u8 2)\n\n(fn f () void\n    (a))\n\n"
                    "(fn g () void\n    (b))\n"));
    // the blank goes above a form's leading comment
    CHECK(results, FormatsTo("(const A u8 1)\n; about f\n(fn f () void (a))",
                             "(const A u8 1)\n\n; about f\n(fn f () void\n    (a))\n"));
    CHECK(results, Format("") == "");
    CHECK(results, Format("\n\n") == "");
}

void TestQuoteMarks(TestResults& results) {
    CHECK(results, Canonical("(f 'x `(a ,b ,@c) (quote y))\n"));
    CHECK(results, FormatsTo("(macro m (a) `(do ,a))", "(macro m (a)\n    `(do\n        ,a))\n"));
}

// all comment text without spaces: comments may join on one line, never get lost
std::string CommentWords(const tisple::SourceFile& file, const Tree& tree) {
    std::string out;
    for (tisple::Span span : tree.comments) {
        for (std::uint32_t i = span.start; i < span.end; i++) {
            if (file.text[i] != ' ' && file.text[i] != '\t' && file.text[i] != '\r') {
                out += file.text[i];
            }
        }
    }
    return out;
}

// format, check the tree survives, then check a second pass changes nothing
bool RoundTrips(const tisple::SourceFile& file, std::string& why) {
    Tree before;
    ReadError error;
    if (!tisple::ReadTree(file, before, error)) {
        why = "does not read";
        return false;
    }
    std::string once = tisple::FormatTree(file, before, true);
    tisple::SourceFile again = tisple::SourceFileFromText(file.path, once);
    Tree after;
    if (!tisple::ReadTree(again, after, error)) {
        why = "formatted text does not read: " + error.message;
        return false;
    }
    if (!tisple::FormsEqual(before.forms, after.forms) ||
        tisple::PrintFlat(before.forms) != tisple::PrintFlat(after.forms)) {
        why = "tree changed";
        return false;
    }
    if (CommentWords(file, before) != CommentWords(again, after)) {
        why = "comments changed";
        return false;
    }
    if (tisple::FormatTree(again, after, true) != once) {
        why = "not idempotent:\n" + once + "\nsecond:\n" + tisple::FormatTree(again, after, true);
        return false;
    }
    return true;
}

void TestExamples(TestResults& results) {
    std::vector<std::string> paths = ExampleFiles();
    CHECK(results, paths.size() >= 8);
    for (const std::string& path : paths) {
        tisple::SourceFile file;
        std::string load_error;
        CHECK(results, tisple::LoadSourceFile(path, file, load_error));
        std::string why;
        bool round_trips = RoundTrips(file, why);
        if (!round_trips) {
            std::fprintf(stderr, "  %s: %s\n", path.c_str(), why.c_str());
        }
        CHECK(results, round_trips);

        // checked in formatted
        std::string formatted;
        ReadError error;
        CHECK(results, tisple::FormatSource(file, formatted, error) && formatted == file.text);

        // mashed onto one line it formats like the file minus comments, blank lines and the
        // author's line breaks in runs of atoms (a data table's rows): the mashed text has
        // none of them, so both pack the values to 100 columns
        Tree tree;
        CHECK(results, tisple::ReadTree(file, tree, error));
        std::string plain = tisple::FormatTree(file, tree, false);
        std::string mashed = Format(tisple::PrintFlat(tree.forms));
        if (mashed != plain) {
            std::fprintf(stderr, "  %s: mashed form formats differently\n", path.c_str());
        }
        CHECK(results, mashed == plain);
        CHECK(results, Format(plain) == plain);
    }
}

// random programs with comments and blank lines anywhere they can go
std::string RandomProgram(std::mt19937& rng) {
    const char* atoms[] = {"fn",     "if",    "do",
                           "for",    "while", "match",
                           "let",    "args",  "enum",
                           "struct", "x",     "y",
                           "0x7F",   "12",    ":in",
                           ":align", "\"s\"", "a_rather_long_name_to_force_breaking_lines_sooner",
                           "+",      "."};
    const char* marks[] = {"'", "`", ",", ",@"};
    auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
    std::string out;
    int depth = 0;
    int length = 10 + pick(60);
    for (int i = 0; i < length || depth > 0; i++) {
        int roll = pick(20);
        bool closing = i >= length || (depth > 0 && roll < 4);
        if (closing) {
            out += ")";
            depth--;
        } else if (roll < 9 && depth < 6) {
            if (pick(8) == 0) {
                out += marks[pick(4)];
            }
            out += "(";
            depth++;
        } else {
            if (pick(10) == 0) {
                out += marks[pick(4)];
            }
            out += atoms[pick(static_cast<int>(std::size(atoms)))];
        }
        int gap = pick(12);
        if (gap == 0) {
            out += " ; note\n";
        } else if (gap == 1) {
            out += "\n; own line\n";
        } else if (gap == 2) {
            out += "\n\n";
        } else if (gap == 3) {
            out += "\n";
        } else {
            out += " ";
        }
    }
    return out;
}

void TestRandomPrograms(TestResults& results) {
    // TISPLE_FORMAT_FUZZ=n runs n programs instead of the quick default
    int rounds = 3000;
    if (const char* env = std::getenv("TISPLE_FORMAT_FUZZ")) {
        rounds = std::atoi(env);
    }
    std::mt19937 rng(1234);
    int failures = 0;
    for (int i = 0; i < rounds; i++) {
        std::string text = RandomProgram(rng);
        tisple::SourceFile file = Text(text);
        Tree tree;
        ReadError error;
        if (!tisple::ReadTree(file, tree, error)) {
            continue; // a mark before ')' and similar: not a formatter case
        }
        std::string why;
        if (!RoundTrips(file, why) && failures < 3) {
            failures++;
            std::fprintf(stderr, "  random program %d: %s\n--- source:\n%s\n", i, why.c_str(),
                         text.c_str());
        }
    }
    CHECK(results, failures == 0);
}

} // namespace

void RunFormatTests(TestResults& results) {
    TestBodyForms(results);
    TestBreaking(results);
    TestDataLists(results);
    TestComments(results);
    TestBlankLines(results);
    TestQuoteMarks(results);
    TestExamples(results);
    TestRandomPrograms(results);
}

} // namespace tisple_tests
