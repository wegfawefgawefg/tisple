#include "test_ide.hpp"

#include "ide/index.hpp"
#include "reader/reader.hpp"

#include <chrono>
#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

using tisple::Definition;
using tisple::EditorIndex;
using tisple::IndexToken;
using tisple::SymbolKind;

bool EndsWith(const std::string& text, const std::string& end) {
    return text.size() >= end.size() &&
           text.compare(text.size() - end.size(), end.size(), end) == 0;
}

std::int32_t FileIndex(const EditorIndex& index, const std::string& suffix) {
    for (std::size_t i = 0; i < index.sources.files.size(); i++) {
        if (EndsWith(index.sources.files[i].path, suffix)) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

// the token starting at line:column (1-based, ASCII sources) of the file ending in suffix
const IndexToken* TokenAt(const EditorIndex& index, const std::string& suffix, std::uint32_t line,
                          std::uint32_t column) {
    std::int32_t file = FileIndex(index, suffix);
    for (const IndexToken& token : index.tokens) {
        if (static_cast<std::int32_t>(token.loc.file) != file) {
            continue;
        }
        tisple::SourcePos pos =
            tisple::PosFromOffset(index.sources.files[token.loc.file], token.loc.span.start);
        if (pos.line == line && pos.column == column) {
            return &token;
        }
    }
    return nullptr;
}

const Definition* DefOf(const EditorIndex& index, const IndexToken* token) {
    if (!token || token->def < 0) {
        return nullptr;
    }
    return &index.definitions[static_cast<std::size_t>(token->def)];
}

// the token at line:column is a kind, its definition has this type, and is declared at
// decl_line:decl_column in the file ending in decl_file
struct Expect {
    const char* file;
    std::uint32_t line;
    std::uint32_t column;
    SymbolKind kind;
    const char* type;
    const char* decl_file;
    std::uint32_t decl_line;
    std::uint32_t decl_column;
};

bool Matches(const EditorIndex& index, const Expect& expect) {
    const IndexToken* token = TokenAt(index, expect.file, expect.line, expect.column);
    const Definition* def = DefOf(index, token);
    if (!def || token->kind != expect.kind || def->type != expect.type) {
        std::fprintf(stderr, "%s:%u:%u: got kind %s type '%s'\n", expect.file, expect.line,
                     expect.column, token ? tisple::SymbolKindName(token->kind) : "(no token)",
                     def ? def->type.c_str() : "(no def)");
        return false;
    }
    if (!expect.decl_file) {
        return !def->has_loc;
    }
    if (!def->has_loc || def->loc.file >= index.sources.files.size()) {
        return false;
    }
    const tisple::SourceFile& file = index.sources.files[def->loc.file];
    tisple::SourcePos pos = tisple::PosFromOffset(file, def->loc.span.start);
    bool ok = EndsWith(file.path, expect.decl_file) && pos.line == expect.decl_line &&
              pos.column == expect.decl_column;
    if (!ok) {
        std::fprintf(stderr, "%s:%u:%u: declared at %s:%u:%u\n", expect.file, expect.line,
                     expect.column, file.path.c_str(), pos.line, pos.column);
    }
    return ok;
}

EditorIndex IndexFile(const std::string& relative) {
    EditorIndex index;
    tisple::IndexProgram(std::string(TISPLE_SOURCE_DIR) + "/" + relative, tisple::LoadOptions(),
                         index);
    return index;
}

void DotTests(TestResults& results) {
    EditorIndex index = IndexFile("examples/dot/main.tsp");
    CHECK(results, index.errors.empty());
    CHECK(results, index.sources.files.size() == 4);
    const char* main = "dot/main.tsp";
    const char* hgr = "gfx/hgr.tsp";
    const Expect expects[] = {
        // (hgr.flip dot.x dot.y): module, function, global, field, global, field
        {main, 35, 6, SymbolKind::Module, "module gfx/hgr", hgr, 1, 1},
        {main, 35, 10, SymbolKind::Function, "fn flip ((x u16) (y u8)) void", hgr, 34, 5},
        {main, 35, 15, SymbolKind::Global, "Dot", main, 10, 6},
        {main, 35, 19, SymbolKind::Field, "field x u16 of Dot", main, 8, 15},
        {main, 35, 25, SymbolKind::Field, "field y u8 of Dot", main, 8, 23},
        // (io_read u8 a2.KBD): a constant from lib/apple2
        {main, 19, 29, SymbolKind::Module, "module apple2", "lib/apple2.tsp", 1, 1},
        {main, 19, 32, SymbolKind::Constant, "u16 = 0xC000 (49152)", "lib/apple2.tsp", 6, 8},
        {main, 24, 13, SymbolKind::Constant, "u8 = 8", "lib/apple2.tsp", 10, 8},
        // locals, enums and their members
        {main, 19, 10, SymbolKind::Local, "u8", main, 19, 10},
        {main, 20, 12, SymbolKind::Local, "u8", main, 19, 10},
        {main, 21, 17, SymbolKind::EnumType, "enum Action (none left right up down)", main, 6, 7},
        {main, 21, 24, SymbolKind::EnumMember, "Action.none = 0", main, 6, 15},
        {main, 42, 29, SymbolKind::EnumMember, "Action.left = 1", main, 6, 20},
        {main, 18, 20, SymbolKind::EnumType, "enum Action (none left right up down)", main, 6, 7},
        {main, 37, 29, SymbolKind::Function, "fn read_action () Action", main, 18, 5},
        // (make Dot :x 140 :y 96): the type and the field names
        {main, 10, 20, SymbolKind::StructType, "struct Dot ((x u16) (y u8))", main, 8, 9},
        {main, 10, 25, SymbolKind::Field, "field x u16 of Dot", main, 8, 15},
        // macros: the call, the argument, the parameters in the template
        {main, 44, 30, SymbolKind::Macro, "macro dec (place)", main, 15, 8},
        {main, 44, 34, SymbolKind::Global, "Dot", main, 10, 6},
        {main, 44, 38, SymbolKind::Field, "field x u16 of Dot", main, 8, 15},
        {main, 16, 12, SymbolKind::Parameter, "parameter place of macro dec", main, 15, 13},
        {main, 12, 13, SymbolKind::Parameter, "parameter place of macro inc", main, 12, 13},
        // the language's own words
        {main, 6, 2, SymbolKind::SpecialForm, "(enum Name (a b (c 7)))", nullptr, 0, 0},
        {main, 8, 17, SymbolKind::PrimitiveType, "u16", nullptr, 0, 0},
        {main, 13, 19, SymbolKind::Builtin, "(+ a b)", nullptr, 0, 0},
        // inside gfx/hgr
        {hgr, 34, 12, SymbolKind::Parameter, "u16", hgr, 34, 12},
        {hgr, 36, 38, SymbolKind::Parameter, "u16", hgr, 34, 12},
        {hgr, 35, 10, SymbolKind::Local, "(ptr u8)", hgr, 35, 10},
        // a table a macro made: declared where its name is written, in the macro call
        {hgr, 35, 41, SymbolKind::Constant, "(array u16 192)", hgr, 14, 15},
        {hgr, 14, 9, SymbolKind::Macro, "macro table (name type count f & options)",
         "lib/tables.tsp", 7, 8},
        // the function the table calls while compiling
        {hgr, 14, 27, SymbolKind::Function, "fn row_start ((y u8)) u16", hgr, 8, 5},
        // macro code: parameters inside and outside the template
        {"lib/tables.tsp", 9, 43, SymbolKind::Parameter, "parameter f of macro table",
         "lib/tables.tsp", 7, 31},
        {"lib/tables.tsp", 14, 23, SymbolKind::Parameter, "parameter row of macro sprite_row",
         "lib/tables.tsp", 13, 20},
    };
    for (const Expect& expect : expects) {
        CHECK(results, Matches(index, expect));
    }

    // declarations are marked, uses are not
    const IndexToken* key_decl = TokenAt(index, main, 19, 10);
    const IndexToken* key_use = TokenAt(index, main, 20, 12);
    CHECK(results, key_decl && key_decl->declaration);
    CHECK(results, key_use && !key_use->declaration);

    // hover text: the leading comment, and for lib constants the trailing one too
    const Definition* flip = DefOf(index, TokenAt(index, main, 35, 10));
    CHECK(results, flip && flip->doc == "xor one pixel, so drawing twice erases it");
    CHECK(results, flip && !flip->library);
    const Definition* kbd = DefOf(index, TokenAt(index, main, 19, 32));
    CHECK(results,
          kbd && kbd->doc == "keyboard\n\nread: last key, bit 7 set while a new key waits");
    CHECK(results, kbd && kbd->library);
    const Definition* x = DefOf(index, TokenAt(index, hgr, 34, 12));
    CHECK(results, x && x->doc.empty());

    // the outline: top-level forms of main.tsp, struct fields and enum members inside
    std::size_t main_symbols = 0;
    bool dot_fields = false;
    bool action_members = false;
    for (const tisple::OutlineSymbol& symbol : index.symbols) {
        if (symbol.file != 0) {
            continue;
        }
        main_symbols++;
        dot_fields = dot_fields || (symbol.name == "Dot" && symbol.children.size() == 2 &&
                                    symbol.children[0].kind == SymbolKind::Field);
        action_members = action_members ||
                         (symbol.name == "Action" && symbol.def >= 0 &&
                          symbol.children.size() == 5 && symbol.children[4].name == "Action.down");
    }
    CHECK(results, main_symbols == 10); // 2 imports, enum, struct, let, 2 macros, 3 fns
    CHECK(results, dot_fields);
    CHECK(results, action_members);
}

void MultiTests(TestResults& results) {
    EditorIndex index = IndexFile("examples/multi/main.tsp");
    CHECK(results, index.errors.empty());
    const char* main = "multi/main.tsp";
    const char* lores = "gfx/lores.tsp";
    const Expect expects[] = {
        // (import gfx/lores gr): the path and the prefix name the module
        {main, 3, 9, SymbolKind::Module, "module gfx/lores", lores, 1, 1},
        {main, 3, 19, SymbolKind::Module, "module gfx/lores", lores, 1, 1},
        {main, 6, 6, SymbolKind::Module, "module gfx/lores", lores, 1, 1},
        {main, 6, 9, SymbolKind::Function, "fn show () void", lores, 9, 5},
        {main, 9, 17, SymbolKind::Function, "fn plot ((x u8) (y u8) (color u8)) void", lores, 16,
         5},
        {main, 9, 22, SymbolKind::Local, "u8", main, 8, 14},
        {main, 9, 34, SymbolKind::Local, "u8", main, 7, 10},
        {lores, 17, 38, SymbolKind::Function, "fn row_base ((row u8)) u16", lores, 6, 5},
        {lores, 7, 14, SymbolKind::Constant, "u16 = 0x0400 (1024)", "lib/apple2.tsp", 71, 8},
    };
    for (const Expect& expect : expects) {
        CHECK(results, Matches(index, expect));
    }
    const Definition* lores_module = DefOf(index, TokenAt(index, main, 6, 6));
    CHECK(results, lores_module && lores_module->doc.find("lo-res graphics") == 0);
    const Definition* plot = DefOf(index, TokenAt(index, main, 9, 17));
    CHECK(results, plot && plot->doc == "even y is the low nibble, odd y the high one");
}

// a missing ')', a bad token, a type error and an unknown name: what can be resolved still is
void BrokenTests(TestResults& results) {
    tisple::LoadOptions options;
    options.lib_dir = "lib0";
    options.files.push_back(tisple::VirtualFile{"t.tsp", "(struct Dot ((x u16) (y u8)))\n"
                                                         "(let dot Dot (make Dot :x 1 :y 2))\n"
                                                         "(fn half ((n u8)) u8\n"
                                                         "    (>> n 1)\n"
                                                         "(fn main () void\n"
                                                         "    (let a u8 (half dot.x))\n"
                                                         "    (let b u8 12z)\n"
                                                         "    (set a (nope a)))\n"});
    EditorIndex index;
    tisple::IndexProgram("t.tsp", options, index);
    std::string errors;
    for (const tisple::Diagnostic& error : index.errors) {
        errors += error.message + "\n";
    }
    CHECK(results, errors.find("this '(' is never closed") != std::string::npos);
    CHECK(results, errors.find("bad digit 'z'") != std::string::npos);
    CHECK(results,
          errors.find("argument n of half needs a u8 but dot.x is u16") != std::string::npos);
    CHECK(results, errors.find("unknown function nope") != std::string::npos);
    const Expect expects[] = {
        {"t.tsp", 4, 9, SymbolKind::Parameter, "u8", "t.tsp", 3, 12},
        {"t.tsp", 6, 16, SymbolKind::Function, "fn half ((n u8)) u8", "t.tsp", 3, 5},
        {"t.tsp", 6, 21, SymbolKind::Global, "Dot", "t.tsp", 2, 6},
        {"t.tsp", 6, 25, SymbolKind::Field, "field x u16 of Dot", "t.tsp", 1, 15},
        {"t.tsp", 8, 10, SymbolKind::Local, "u8", "t.tsp", 6, 10},
        // the checker gives up on (nope a) at its head; the argument is still found
        {"t.tsp", 8, 18, SymbolKind::Local, "u8", "t.tsp", 6, 10},
    };
    for (const Expect& expect : expects) {
        CHECK(results, Matches(index, expect));
    }
    CHECK(results, TokenAt(index, "t.tsp", 8, 13) == nullptr); // nope resolves to nothing
    std::string json = tisple::IndexJson(index);
    CHECK(results, json.find("\"message\":\"unknown function nope\"") != std::string::npos);

    // half typed: a call short of its arguments and an if short of its arms still index
    tisple::LoadOptions typing;
    typing.lib_dir = "lib0";
    typing.files.push_back(tisple::VirtualFile{"t.tsp", "(struct Dot ((x u16) (y u8)))\n"
                                                        "(let dot Dot (make Dot :x 1 :y 2))\n"
                                                        "(fn two ((a u16) (b u8)) void)\n"
                                                        "(fn main () void\n"
                                                        "    (let k u8 1)\n"
                                                        "    (two dot.x\n"
                                                        "    (if (< k\n"});
    EditorIndex typed;
    tisple::IndexProgram("t.tsp", typing, typed);
    const Expect typed_expects[] = {
        {"t.tsp", 6, 6, SymbolKind::Function, "fn two ((a u16) (b u8)) void", "t.tsp", 3, 5},
        {"t.tsp", 6, 10, SymbolKind::Global, "Dot", "t.tsp", 2, 6},
        {"t.tsp", 6, 14, SymbolKind::Field, "field x u16 of Dot", "t.tsp", 1, 15},
        {"t.tsp", 7, 12, SymbolKind::Local, "u8", "t.tsp", 5, 10},
    };
    for (const Expect& expect : typed_expects) {
        CHECK(results, Matches(typed, expect));
    }

    // an entry that cannot be read still gives JSON with its error
    EditorIndex missing;
    tisple::IndexProgram("no/such/file.tsp", tisple::LoadOptions(), missing);
    CHECK(results, missing.errors.size() == 1 && missing.sources.files.size() == 1);
    CHECK(results, tisple::IndexJson(missing).find("cannot open") != std::string::npos);
}

void RecoveringReaderTests(TestResults& results) {
    // an extra ')', a form a ')' short, an open string: every form still comes out
    tisple::SourceFile file = tisple::SourceFileFromText(
        "t.tsp", "(const A u8 1))\n(fn f () void\n    (g\n(const B u8 2)\n(const C u8 \"x)\n");
    tisple::Tree tree;
    std::vector<tisple::ReadError> errors;
    tisple::ReadTreeRecovering(file, tree, errors);
    CHECK(results, tree.forms.size() == 4);
    CHECK(results, errors.size() == 4);
    CHECK(results, tree.forms.size() == 4 && tree.forms[1].items.size() == 5);
    CHECK(results, tree.forms.size() == 4 && tree.forms[3].items.size() == 3);
    // a file that reads cleanly reads the same as ReadTree
    tisple::SourceFile clean = tisple::SourceFileFromText("t.tsp", "(a (b c)) 'd");
    tisple::Tree plain;
    tisple::ReadError error;
    CHECK(results, tisple::ReadTree(clean, plain, error));
    errors.clear();
    tisple::ReadTreeRecovering(clean, tree, errors);
    CHECK(results, errors.empty() && tisple::FormsEqual(plain.forms, tree.forms));
}

// the editor runs this on every keystroke, so it must stay quick
void TimingTests(TestResults& results) {
    const int runs = 10;
    auto start = std::chrono::steady_clock::now();
    std::size_t tokens = 0;
    for (int i = 0; i < runs; i++) {
        EditorIndex index = IndexFile("examples/dot/main.tsp");
        tokens += index.tokens.size();
        tokens += tisple::IndexJson(index).size() > 0 ? 0 : 1;
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count() /
                runs;
    // a few ms in a debug build; the bound leaves room for a loaded machine
    std::printf("index: examples/dot/main.tsp in %.1f ms (load, expand, check, JSON)\n", ms);
    CHECK(results, ms < 100.0);
    CHECK(results, tokens > 0);
}

} // namespace

void RunIndexTests(TestResults& results) {
    DotTests(results);
    MultiTests(results);
    BrokenTests(results);
    RecoveringReaderTests(results);
    TimingTests(results);
}

} // namespace tisple_tests
