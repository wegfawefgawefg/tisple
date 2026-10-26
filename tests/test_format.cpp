#include "reader/format.hpp"
#include "test_reader.hpp"

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
    CHECK(results, Canonical("(fn fill (args (dst (ptr u8)) (count u16) (value u8))\n"
                             "    (for i 0 count\n"
                             "        (store (+ dst i) value)))\n"));
    CHECK(results, Canonical("(fn read_key\n"
                             "    (let key (io_read u8 0xC000))\n"
                             "    (bit_and key 0x7F))\n"));
    // if arms always get their own lines, even when short
    CHECK(results,
          FormatsTo("(fn f (if c (a) (b)))", "(fn f\n    (if c\n        (a)\n        (b)))\n"));
    CHECK(results, Canonical("(while (not (ready)))\n"));
    CHECK(results, Canonical("(do)\n"));
    CHECK(results, Canonical("(match a\n"
                             "    (Action.left (step 255))\n"
                             "    (Action.fire (do\n"
                             "        (shoot)\n"
                             "        (click)))\n"
                             "    (_ (do)))\n"));
    // a body form as the last argument hangs off the head line
    CHECK(results, Canonical("(let m (if (< a b)\n    a\n    b))\n"));
    CHECK(results, Canonical("(macro swap (args a b)\n"
                             "    `(do\n"
                             "        (let tmp# ,a)\n"
                             "        (set ,a ,b)\n"
                             "        (set ,b tmp#)))\n"));
}

void TestBreaking(TestResults& results) {
    std::string name(40, 'a');
    // general form: head and first argument stay, the rest go one per line at +4
    CHECK(results, Canonical("(fn f\n"
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
    std::string table = Format("(const T (array u16 40)" + atoms + ")");
    CHECK(results, table.find("(const T\n    (array u16 40)\n    0x1000 0x1001") == 0);
    CHECK(results, Format(Mash(table)) == table);
    // args breaks with every parameter on its own line
    std::string long_args =
        "(fn draw (args (dst (ptr u8)) (src (ptr u8)) (" + name + " u8) (count u8)) (copy))";
    CHECK(results, FormatsTo(long_args, "(fn draw (args\n"
                                        "        (dst (ptr u8))\n"
                                        "        (src (ptr u8))\n"
                                        "        (" +
                                            name +
                                            " u8)\n"
                                            "        (count u8))\n"
                                            "    (copy))\n"));
    // enum and struct: one line when they fit, else one member per line
    CHECK(results, Canonical("(enum Action none left right fire)\n"));
    CHECK(results, Canonical("(struct Sprite (x u8) (y u8))\n"));
    CHECK(results,
          Canonical("(enum Big\n    " + name + "\n    " + name + "_b\n    " + name + "_c)\n"));
    // a keyword and its value stay together
    CHECK(results, Canonical("(let " + name + "\n    (array u8 200)\n    :in " + name +
                             "\n    :align 256\n    :boundary 256)\n"));
    // closing parens count toward the 100 columns
    std::string fits(92, 'x');
    CHECK(results, Canonical("(a (b " + fits + "))\n"));
    CHECK(results, Format("(a (b " + fits + "x))").find('\n') < 20);
}

void TestComments(TestResults& results) {
    // trailing comments stay on their line and force the break after them
    CHECK(results, FormatsTo("(enum Command none left ; arrow\n fire)",
                             "(enum Command\n    none\n    left ; arrow\n    fire)\n"));
    // trailing comments on consecutive lines line up
    CHECK(results, FormatsTo("(fn show\n (a) ; one\n (bbbb) ; two\n (c))\n",
                             "(fn show\n    (a)    ; one\n    (bbbb) ; two\n    (c))\n"));
    // a comment after the last item moves past the closing parens
    CHECK(results, FormatsTo("(fn f\n (a) ; done\n)", "(fn f\n    (a)) ; done\n"));
    CHECK(results, FormatsTo("(a b ; c\n)", "(a b) ; c\n"));
    // own-line comments keep their place and indent
    CHECK(results, FormatsTo("(fn f\n; first\n  (a)\n      ; second\n (b))",
                             "(fn f\n    ; first\n    (a)\n    ; second\n    (b))\n"));
    // a comment before ')' stays inside, with ')' on its own line
    CHECK(results, FormatsTo("(fn f\n (a)\n ; todo\n )", "(fn f\n    (a)\n    ; todo\n)\n"));
    // a comment right after '(' goes to the end of the head line
    CHECK(results, FormatsTo("(fn ; c\n f\n (a))", "(fn ; c\n        f\n    (a))\n"));
    CHECK(results, FormatsTo("( ; c\nfn f (a))", "(fn f ; c\n    (a))\n"));
    // file-level comments
    CHECK(results, FormatsTo("; top\n\n\n\n(a)\n; end", "; top\n\n(a)\n; end\n"));
    CHECK(results, FormatsTo("(a) ; x\n(b) ; y\n", "(a) ; x\n(b) ; y\n"));
}

void TestBlankLines(TestResults& results) {
    // single blank lines are kept, runs collapse, none at the start of a body
    CHECK(results,
          FormatsTo("(fn f\n\n (a)\n\n\n (b)\n (c))", "(fn f\n    (a)\n\n    (b)\n    (c))\n"));
    // multi-line top-level forms always get a blank line around them
    CHECK(results, FormatsTo("(const A u8 1)\n(const B u8 2)\n(fn f (a))\n(fn g (b))",
                             "(const A u8 1)\n(const B u8 2)\n\n(fn f\n    (a))\n\n"
                             "(fn g\n    (b))\n"));
    // the blank goes above a form's leading comment
    CHECK(results, FormatsTo("(const A u8 1)\n; about f\n(fn f (a))",
                             "(const A u8 1)\n\n; about f\n(fn f\n    (a))\n"));
    CHECK(results, Format("") == "");
    CHECK(results, Format("\n\n") == "");
}

void TestQuoteMarks(TestResults& results) {
    CHECK(results, Canonical("(f 'x `(a ,b ,@c) (quote y))\n"));
    CHECK(results,
          FormatsTo("(macro m (args a) `(do ,a))", "(macro m (args a)\n    `(do\n        ,a))\n"));
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

        // mashed onto one line it formats like the file minus comments and blank lines
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
    TestComments(results);
    TestBlankLines(results);
    TestQuoteMarks(results);
    TestExamples(results);
    TestRandomPrograms(results);
}

} // namespace tisple_tests
