#include "macro/value.hpp"

#include <cstdio>

namespace tisple {

Value IntValue(std::int64_t integer, std::int32_t module) {
    Value value;
    value.kind = ValueKind::Int;
    value.integer = integer;
    value.module = module;
    return value;
}

Value FloatValue(double real, std::int32_t module) {
    Value value;
    value.kind = ValueKind::Float;
    value.real = real;
    value.module = module;
    return value;
}

Value BoolValue(bool truth, std::int32_t module) {
    Value value;
    value.kind = ValueKind::Bool;
    value.integer = truth ? 1 : 0;
    value.module = module;
    return value;
}

Value SymbolValue(const std::string& text, std::int32_t module) {
    Value value;
    value.kind = ValueKind::Symbol;
    value.text = text;
    value.module = module;
    return value;
}

Value ListValue(std::vector<Value> items, std::int32_t module) {
    Value value;
    value.kind = ValueKind::List;
    value.items = std::move(items);
    value.module = module;
    return value;
}

Value NodeToValue(const Node& node, bool written, std::uint32_t file, std::int32_t module) {
    Value value;
    value.written = written;
    value.file = file;
    value.span = node.span;
    value.expansion = node.expansion;
    value.module = module;
    switch (node.kind) {
    case NodeKind::List:
        value.kind = ValueKind::List;
        value.mark = node.mark;
        value.items.reserve(node.items.size());
        for (const Node& item : node.items) {
            value.items.push_back(NodeToValue(item, written, file, module));
        }
        break;
    case NodeKind::Symbol:
        value.kind = ValueKind::Symbol;
        value.text = node.text;
        if (node.text == "true" || node.text == "false") {
            value.kind = ValueKind::Bool;
            value.integer = node.text == "true" ? 1 : 0;
        }
        break;
    case NodeKind::Keyword:
        value.kind = ValueKind::Keyword;
        value.text = node.text;
        break;
    case NodeKind::Integer:
        value.kind = ValueKind::Int;
        value.integer = node.value;
        value.spelling = node.text;
        break;
    case NodeKind::String:
        value.kind = ValueKind::String;
        value.text = node.bytes;
        value.spelling = node.text;
        break;
    }
    return value;
}

namespace {

std::string QuotedString(const std::string& bytes) {
    std::string out = "\"";
    for (char c : bytes) {
        unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (u < 0x20 || u >= 0x7F) {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "\\x%02X", u);
            out += hex;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

// the expansion whose module is where the value's names resolve
std::uint32_t StampExpansion(Stamp& stamp, std::int32_t module) {
    if (module < 0 || module == stamp.module || !stamp.sources) {
        return stamp.expansion;
    }
    for (const auto& other : stamp.others) {
        if (other.first == module) {
            return other.second;
        }
    }
    stamp.sources->expansions.push_back(Expansion{stamp.macro, stamp.call, module});
    std::uint32_t id = static_cast<std::uint32_t>(stamp.sources->expansions.size());
    stamp.others.push_back({module, id});
    return id;
}

} // namespace

bool ValueToNode(const Value& value, Stamp& stamp, Node& out, std::string& why) {
    out = Node();
    if (value.written) {
        out.span = value.span;
        out.expansion = value.expansion;
    } else {
        out.span = stamp.call.span;
        out.expansion = StampExpansion(stamp, value.module);
    }
    switch (value.kind) {
    case ValueKind::Void:
        why = "it gives nothing to paste in; end its body with the forms, like `(...)";
        return false;
    case ValueKind::Float:
        why = "the float " + ShowValue(value) + " can't go into code; write (round x) or (floor x)";
        return false;
    case ValueKind::Fn:
        why = "a fn can't go into code; call it";
        return false;
    case ValueKind::Int:
        out.kind = NodeKind::Integer;
        out.value = value.integer;
        out.text = value.spelling.empty() ? std::to_string(value.integer) : value.spelling;
        return true;
    case ValueKind::Bool:
        out.kind = NodeKind::Symbol;
        out.text = value.integer != 0 ? "true" : "false";
        return true;
    case ValueKind::Symbol:
        out.kind = NodeKind::Symbol;
        out.text = value.text;
        return true;
    case ValueKind::Keyword:
        out.kind = NodeKind::Keyword;
        out.text = value.text;
        return true;
    case ValueKind::String:
        out.kind = NodeKind::String;
        out.bytes = value.text;
        out.text = value.spelling.empty() ? QuotedString(value.text) : value.spelling;
        return true;
    case ValueKind::List:
        out.kind = NodeKind::List;
        out.mark = value.mark;
        out.items.resize(value.items.size());
        for (std::size_t i = 0; i < value.items.size(); i++) {
            if (!ValueToNode(value.items[i], stamp, out.items[i], why)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

const char* KindName(ValueKind kind) {
    switch (kind) {
    case ValueKind::Void:
        return "nothing";
    case ValueKind::Int:
        return "int";
    case ValueKind::Float:
        return "float";
    case ValueKind::Bool:
        return "bool";
    case ValueKind::Symbol:
        return "symbol";
    case ValueKind::Keyword:
        return "keyword";
    case ValueKind::String:
        return "string";
    case ValueKind::List:
        return "list";
    case ValueKind::Fn:
        return "fn";
    }
    return "nothing";
}

std::string ShowValue(const Value& value) {
    switch (value.kind) {
    case ValueKind::Void:
        return "nothing";
    case ValueKind::Int:
        return std::to_string(value.integer);
    case ValueKind::Float: {
        char text[64];
        std::snprintf(text, sizeof(text), "%.17g", value.real);
        return text;
    }
    case ValueKind::Bool:
        return value.integer != 0 ? "true" : "false";
    case ValueKind::Symbol:
    case ValueKind::Keyword:
        return value.text;
    case ValueKind::String:
        return QuotedString(value.text);
    case ValueKind::Fn:
        return "(fn ...)";
    case ValueKind::List:
        break;
    }
    if (value.mark != QuoteMark::None && value.items.size() == 2) {
        return QuoteMarkText(value.mark) + ShowValue(value.items[1]);
    }
    std::string out = "(";
    for (std::size_t i = 0; i < value.items.size(); i++) {
        out += (i == 0 ? "" : " ") + ShowValue(value.items[i]);
        if (out.size() > 60) {
            return out + " ...)";
        }
    }
    return out + ")";
}

std::string DescribeValue(const Value& value) {
    switch (value.kind) {
    case ValueKind::Int:
    case ValueKind::Float:
    case ValueKind::Bool:
    case ValueKind::String:
    case ValueKind::Void:
        return ShowValue(value);
    case ValueKind::Fn:
        return "a fn";
    default:
        return std::string("the ") + KindName(value.kind) + " " + ShowValue(value);
    }
}

bool ValuesEqual(const Value& a, const Value& b) {
    bool a_number = a.kind == ValueKind::Int || a.kind == ValueKind::Float;
    bool b_number = b.kind == ValueKind::Int || b.kind == ValueKind::Float;
    if (a_number && b_number && a.kind != b.kind) {
        double x = a.kind == ValueKind::Int ? static_cast<double>(a.integer) : a.real;
        double y = b.kind == ValueKind::Int ? static_cast<double>(b.integer) : b.real;
        return x == y;
    }
    if (a.kind != b.kind) {
        return false;
    }
    switch (a.kind) {
    case ValueKind::Void:
        return true;
    case ValueKind::Int:
    case ValueKind::Bool:
        return a.integer == b.integer;
    case ValueKind::Float:
        return a.real == b.real;
    case ValueKind::Symbol:
    case ValueKind::Keyword:
    case ValueKind::String:
        return a.text == b.text;
    case ValueKind::Fn:
        return a.fn == b.fn;
    case ValueKind::List:
        break;
    }
    if (a.items.size() != b.items.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.items.size(); i++) {
        if (!ValuesEqual(a.items[i], b.items[i])) {
            return false;
        }
    }
    return true;
}

} // namespace tisple
