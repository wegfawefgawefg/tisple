#include "reader/editor_syntax.hpp"
#include "reader/keywords.hpp"
#include "reader/show.hpp"
#include "test_reader.hpp"

#include <fstream>
#include <sstream>

namespace tisple_tests {

namespace {

using tisple::SymbolClass;

std::string StripEscapes(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\x1b') {
            while (i < text.size() && text[i] != 'm') {
                i++;
            }
            continue;
        }
        out += text[i];
    }
    return out;
}

std::string Show(const std::string& text, bool color, bool& ok) {
    std::string out;
    ok = tisple::RenderShow(Text(text), color, out);
    return out;
}

std::string ReadWhole(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

void TestPlainAndColor(TestResults& results) {
    std::string source = "; hi\n(fn f (args (x u8))\n    (+ x 0x10 \"s\" :in Sprite MAX))\n";
    bool ok = false;
    std::string plain = Show(source, false, ok);
    CHECK(results, ok);
    CHECK(results,
          plain == "1  ; hi\n2  (fn f (args (x u8))\n3      (+ x 0x10 \"s\" :in Sprite MAX))\n");
    CHECK(results, plain.find('\x1b') == std::string::npos);

    std::string color = Show(source, true, ok);
    CHECK(results, ok && StripEscapes(color) == plain);
    CHECK(results, color.find("\x1b[1;38;5;176mfn\x1b[0m") != std::string::npos);  // special
    CHECK(results, color.find("\x1b[38;5;80mu8\x1b[0m") != std::string::npos);     // type
    CHECK(results, color.find("\x1b[38;5;80mSprite\x1b[0m") != std::string::npos); // by spelling
    CHECK(results, color.find("\x1b[38;5;222mMAX\x1b[0m") != std::string::npos);   // constant
    CHECK(results, color.find("\x1b[38;5;215m0x10\x1b[0m") != std::string::npos);  // number
    CHECK(results, color.find("\x1b[38;5;217m:in\x1b[0m") != std::string::npos);   // keyword
    // parentheses by depth: (fn is depth 0, (args depth 1, (x depth 2
    CHECK(results, color.find("\x1b[38;5;179m(\x1b[0m\x1b[1;38;5;176mfn") != std::string::npos);
    CHECK(results, color.find("\x1b[38;5;140m(\x1b[0m\x1b[1;38;5;176margs") != std::string::npos);

    // line numbers pad to the widest
    std::string many;
    for (int i = 0; i < 10; i++) {
        many += "(a)\n";
    }
    std::string shown = Show(many, false, ok);
    CHECK(results, shown.find(" 1  (a)\n") == 0 && shown.find("10  (a)\n") != std::string::npos);
}

void TestErrors(TestResults& results) {
    bool ok = true;
    // a paren error colors through the error's line, marks it, dims the rest
    std::string source = "(fn a\n    (b)\n(fn c\n    (d))\n";
    std::string color = Show(source, true, ok);
    CHECK(results, !ok);
    std::string plain = StripEscapes(color);
    CHECK(results, plain.find("1  (fn a\n   ^ error: this '(' is never closed\n") == 0);
    CHECK(results, plain.find("3  (fn c\n   ^ note: ") != std::string::npos);
    CHECK(results, color.find("\x1b[1;97;41m(\x1b[0m") != std::string::npos);
    CHECK(results, color.find("\x1b[38;5;240m    (b)") != std::string::npos);

    // a lex error colors up to the bad character
    color = Show("(let x 0x1G)\n(b)\n", true, ok);
    CHECK(results, !ok);
    plain = StripEscapes(color);
    CHECK(results, plain.find("1  (let x 0x1G)\n             ^ error: bad digit 'G'") == 0);
    CHECK(results, color.find("\x1b[38;5;240m(b)") != std::string::npos);

    // a stray ')' is marked where it is
    plain = Show("(a))\n", false, ok);
    CHECK(results, !ok && plain.find("1  (a))\n      ^ error: ')' has no matching '('") == 0);
}

void TestKeywords(TestResults& results) {
    CHECK(results, tisple::ClassifySymbol("fn") == SymbolClass::SpecialForm);
    CHECK(results, tisple::ClassifySymbol(".") == SymbolClass::SpecialForm);
    CHECK(results, tisple::ClassifySymbol("macro") == SymbolClass::SpecialForm);
    CHECK(results, tisple::ClassifySymbol("import") == SymbolClass::SpecialForm);
    CHECK(results, tisple::ClassifySymbol("match") == SymbolClass::SpecialForm);
    CHECK(results, tisple::ClassifySymbol("u16") == SymbolClass::Type);
    CHECK(results, tisple::ClassifySymbol("io_write") == SymbolClass::Builtin);
    CHECK(results, tisple::ClassifySymbol("<=") == SymbolClass::Builtin);
    CHECK(results, tisple::ClassifySymbol("true") == SymbolClass::Constant);
    CHECK(results, tisple::ClassifySymbol("Sprite") == SymbolClass::Type);
    CHECK(results, tisple::ClassifySymbol("gfx.Sprite") == SymbolClass::Type);
    CHECK(results, tisple::ClassifySymbol("MAX_SPRITES") == SymbolClass::Constant);
    CHECK(results, tisple::ClassifySymbol("hgr.PAGE1") == SymbolClass::Constant);
    CHECK(results, tisple::ClassifySymbol("sprite_count") == SymbolClass::Name);
    CHECK(results, tisple::ClassifySymbol("hgr.plot") == SymbolClass::Name);
    CHECK(results, tisple::ClassifySymbol("tmp#") == SymbolClass::Name);
}

void TestEditorFiles(TestResults& results) {
    // the checked-in grammars match the keyword lists; `tisple gen-editors editors` fixes them
    std::string vscode =
        ReadWhole(SourcePath(std::string("editors/") + tisple::kVsCodeGrammarPath));
    std::string nvim = ReadWhole(SourcePath(std::string("editors/") + tisple::kNvimSyntaxPath));
    CHECK(results, vscode == tisple::VsCodeGrammar());
    CHECK(results, nvim == tisple::NvimSyntax());
    CHECK(results, vscode.find("|match|") != std::string::npos);
}

void TestExamplesShow(TestResults& results) {
    for (const std::string& path : ExampleFiles()) {
        tisple::SourceFile file;
        std::string error;
        CHECK(results, tisple::LoadSourceFile(path, file, error));
        std::string plain;
        std::string color;
        CHECK(results, tisple::RenderShow(file, false, plain));
        CHECK(results, tisple::RenderShow(file, true, color));
        CHECK(results, StripEscapes(color) == plain);
    }
}

} // namespace

void RunShowTests(TestResults& results) {
    TestPlainAndColor(results);
    TestErrors(results);
    TestKeywords(results);
    TestEditorFiles(results);
    TestExamplesShow(results);
}

} // namespace tisple_tests
