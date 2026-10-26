#include "ide/index_internal.hpp"

#include <algorithm>

namespace tisple {

namespace {

const std::string& HeadOf(const Node& node) {
    static const std::string kEmpty;
    if (node.kind != NodeKind::List || node.mark != QuoteMark::None || node.items.empty() ||
        node.items[0].kind != NodeKind::Symbol) {
        return kEmpty;
    }
    return node.items[0].text;
}

bool TopLevelKind(const std::string& head, SymbolKind& kind) {
    if (head == "fn") {
        kind = SymbolKind::Function;
    } else if (head == "macro") {
        kind = SymbolKind::Macro;
    } else if (head == "const") {
        kind = SymbolKind::Constant;
    } else if (head == "let") {
        kind = SymbolKind::Global;
    } else if (head == "struct") {
        kind = SymbolKind::StructType;
    } else if (head == "enum") {
        kind = SymbolKind::EnumType;
    } else if (head == "import") {
        kind = SymbolKind::Module;
    } else {
        return false;
    }
    return true;
}

OutlineSymbol Entry(std::uint32_t file, const Node& form, const Node& name, SymbolKind kind,
                    const std::string& text) {
    OutlineSymbol symbol;
    symbol.name = text;
    symbol.kind = kind;
    symbol.file = file;
    symbol.range = form.span;
    symbol.name_span = name.span;
    return symbol;
}

// (struct Dot ((x u16) (y u8))) and (enum Action (none (fire 7))): one child per entry
void AddChildren(OutlineSymbol& parent, const Node& form) {
    if (form.items.size() < 3 || form.items[2].kind != NodeKind::List) {
        return;
    }
    bool is_struct = parent.kind == SymbolKind::StructType;
    for (const Node& item : form.items[2].items) {
        const Node* name = &item;
        if (item.kind == NodeKind::List && !item.items.empty()) {
            name = &item.items[0];
        }
        if (name->kind != NodeKind::Symbol) {
            continue;
        }
        SymbolKind kind = is_struct ? SymbolKind::Field : SymbolKind::EnumMember;
        std::string text = is_struct ? name->text : parent.name + "." + name->text;
        parent.children.push_back(Entry(parent.file, item, *name, kind, text));
    }
}

bool TokenBefore(const IndexToken& token, const OutlineSymbol& symbol) {
    if (token.loc.file != symbol.file) {
        return token.loc.file < symbol.file;
    }
    return token.loc.span.start < symbol.name_span.start;
}

void LinkSymbol(const EditorIndex& index, OutlineSymbol& symbol) {
    // tokens are sorted by file and position
    auto token = std::lower_bound(index.tokens.begin(), index.tokens.end(), symbol, TokenBefore);
    if (token != index.tokens.end() && token->loc.file == symbol.file &&
        token->loc.span.start == symbol.name_span.start) {
        symbol.def = token->def;
    }
    for (OutlineSymbol& child : symbol.children) {
        LinkSymbol(index, child);
    }
}

} // namespace

void BuildOutline(const LoadedProgram& loaded, std::vector<OutlineSymbol>& out) {
    for (const Module& module : loaded.modules) {
        for (const Node& form : module.tree.forms) {
            SymbolKind kind = SymbolKind::Function;
            if (!TopLevelKind(HeadOf(form), kind) || form.items.size() < 2 ||
                form.items[1].kind != NodeKind::Symbol) {
                continue;
            }
            const Node& name = form.items[1];
            OutlineSymbol symbol = Entry(module.file, form, name, kind, name.text);
            if (kind == SymbolKind::StructType || kind == SymbolKind::EnumType) {
                AddChildren(symbol, form);
            }
            out.push_back(std::move(symbol));
        }
    }
}

void LinkOutline(EditorIndex& index) {
    for (OutlineSymbol& symbol : index.symbols) {
        LinkSymbol(index, symbol);
    }
}

} // namespace tisple
