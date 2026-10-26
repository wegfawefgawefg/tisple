#include "ide/index_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace tisple {

namespace {

void AddString(std::string& out, const std::string& text) {
    out += '"';
    for (char c : text) {
        unsigned char byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (byte < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
            out += escaped;
        } else {
            out += c;
        }
    }
    out += '"';
}

void AddKey(std::string& out, const char* key) {
    out += '"';
    out += key;
    out += "\":";
}

void AddNumber(std::string& out, const char* key, std::int64_t value) {
    AddKey(out, key);
    out += std::to_string(value);
    out += ',';
}

void AddText(std::string& out, const char* key, const std::string& value) {
    AddKey(out, key);
    AddString(out, value);
    out += ',';
}

void AddBool(std::string& out, const char* key, bool value) {
    AddKey(out, key);
    out += value ? "true," : "false,";
}

// drops the comma after the last member
void CloseObject(std::string& out) {
    if (!out.empty() && out.back() == ',') {
        out.pop_back();
    }
    out += '}';
}

const SourceFile* FileAt(const EditorIndex& index, std::uint32_t file) {
    return file < index.sources.files.size() ? &index.sources.files[file] : nullptr;
}

// "range":{"line":..,"column":..,"end_line":..,"end_column":..}, 1-based, end exclusive
void AddRange(std::string& out, const char* key, const EditorIndex& index, std::uint32_t file,
              Span span) {
    const SourceFile* source = FileAt(index, file);
    SourcePos start;
    SourcePos end;
    if (source) {
        std::uint32_t size = static_cast<std::uint32_t>(source->text.size());
        start = EditorPos(*source, std::min(span.start, size));
        end = EditorPos(*source, std::min(std::max(span.end, span.start), size));
    }
    AddKey(out, key);
    out += '{';
    AddNumber(out, "line", start.line);
    AddNumber(out, "column", start.column);
    AddNumber(out, "end_line", end.line);
    AddNumber(out, "end_column", end.column);
    CloseObject(out);
    out += ',';
}

std::string AbsolutePath(const std::string& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        return path;
    }
    return absolute.lexically_normal().string();
}

void AddDefinitionJson(std::string& out, const EditorIndex& index, const Definition& def) {
    out += '{';
    AddText(out, "name", def.name);
    AddText(out, "qualified", def.qualified);
    AddText(out, "kind", SymbolKindName(def.kind));
    AddText(out, "type", def.type);
    AddText(out, "doc", def.doc);
    AddBool(out, "library", def.library);
    if (def.has_loc) {
        AddNumber(out, "file", def.loc.file);
        AddRange(out, "range", index, def.loc.file, def.loc.span);
    } else {
        AddNumber(out, "file", -1);
    }
    CloseObject(out);
}

void AddTokenJson(std::string& out, const EditorIndex& index, const IndexToken& token) {
    const SourceFile* source = FileAt(index, token.loc.file);
    SourcePos start;
    SourcePos end;
    if (source) {
        start = EditorPos(*source, token.loc.span.start);
        end = EditorPos(*source, token.loc.span.end);
    }
    out += '{';
    AddNumber(out, "file", token.loc.file);
    AddNumber(out, "line", start.line);
    AddNumber(out, "column", start.column);
    AddNumber(out, "length", end.column - start.column);
    AddText(out, "kind", SymbolKindName(token.kind));
    AddNumber(out, "def", token.def);
    AddBool(out, "declaration", token.declaration);
    CloseObject(out);
}

void AddSymbolJson(std::string& out, const EditorIndex& index, const OutlineSymbol& symbol) {
    out += '{';
    AddText(out, "name", symbol.name);
    AddText(out, "kind", SymbolKindName(symbol.kind));
    AddNumber(out, "file", symbol.file);
    AddRange(out, "range", index, symbol.file, symbol.range);
    AddRange(out, "selection", index, symbol.file, symbol.name_span);
    AddNumber(out, "def", symbol.def);
    AddKey(out, "children");
    out += '[';
    for (std::size_t i = 0; i < symbol.children.size(); i++) {
        out += i == 0 ? "" : ",";
        AddSymbolJson(out, index, symbol.children[i]);
    }
    out += "]}";
}

void AddErrorJson(std::string& out, const EditorIndex& index, const Diagnostic& error) {
    out += '{';
    AddNumber(out, "file", error.loc.file);
    AddRange(out, "range", index, error.loc.file, error.loc.span);
    AddText(out, "message", error.message);
    std::uint32_t expansion = error.loc.expansion;
    if (expansion != 0 && expansion <= index.sources.expansions.size()) {
        AddText(out, "macro", index.sources.expansions[expansion - 1].macro);
    }
    CloseObject(out);
}

} // namespace

SourcePos EditorPos(const SourceFile& file, std::uint32_t offset) {
    SourcePos pos = PosFromOffset(file, offset);
    std::uint32_t column = 1;
    for (std::uint32_t i = file.line_starts[pos.line - 1]; i < offset && i < file.text.size();
         i++) {
        unsigned char byte = static_cast<unsigned char>(file.text[i]);
        if ((byte & 0xC0) == 0x80) {
            continue; // UTF-8 continuation byte
        }
        column += byte >= 0xF0 ? 2 : 1; // four-byte characters are two UTF-16 units
    }
    pos.column = column;
    return pos;
}

std::string IndexJson(const EditorIndex& index) {
    std::string out = "{\"files\":[";
    for (std::size_t i = 0; i < index.sources.files.size(); i++) {
        out += i == 0 ? "" : ",";
        AddString(out, AbsolutePath(index.sources.files[i].path));
    }
    out += "],\n\"definitions\":[";
    for (std::size_t i = 0; i < index.definitions.size(); i++) {
        out += i == 0 ? "\n" : ",\n";
        AddDefinitionJson(out, index, index.definitions[i]);
    }
    out += "],\n\"tokens\":[";
    for (std::size_t i = 0; i < index.tokens.size(); i++) {
        out += i == 0 ? "\n" : ",\n";
        AddTokenJson(out, index, index.tokens[i]);
    }
    out += "],\n\"symbols\":[";
    for (std::size_t i = 0; i < index.symbols.size(); i++) {
        out += i == 0 ? "\n" : ",\n";
        AddSymbolJson(out, index, index.symbols[i]);
    }
    out += "],\n\"errors\":[";
    for (std::size_t i = 0; i < index.errors.size(); i++) {
        out += i == 0 ? "\n" : ",\n";
        AddErrorJson(out, index, index.errors[i]);
    }
    out += "]}\n";
    return out;
}

} // namespace tisple
