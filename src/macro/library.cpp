#include "macro/eval_internal.hpp"

#include <cmath>
#include <cstdint>

namespace tisple {

namespace {

// keep in step with docs/language.md (Macros)
const char* const kLibraryNames[] = {
    "list",  "len",   "nth",  "head", "tail", "append", "map",   "range", "repeat", "symbol",
    "chars", "error", "kind", "sin",  "cos",  "sqrt",   "floor", "round", "float",  "pi",
};

const char* const kOperators[] = {
    "+", "-", "*", "/", "%", "&", "|", "^", "~", "<<", ">>", "=", "!=", "<", "<=", ">", ">=",
};

// the most items one list may hold: more than any Apple II table needs
constexpr std::size_t kMaxListItems = 131072;

struct LibCall {
    MacroRun& run;
    Frame& frame;
    const std::string& name;
    std::vector<Value>& args;
    const Node& at;
};

Value Fail(LibCall& c, const std::string& message) {
    RunFail(c.run, c.frame, c.at, message);
    return Value();
}

bool Count(LibCall& c, std::size_t count, const char* shape) {
    if (c.args.size() == count) {
        return true;
    }
    Fail(c, std::string("write ") + shape);
    return false;
}

bool IsNumber(const Value& value) {
    return value.kind == ValueKind::Int || value.kind == ValueKind::Float;
}

double AsDouble(const Value& value) {
    return value.kind == ValueKind::Int ? static_cast<double>(value.integer) : value.real;
}

bool NeedInts(LibCall& c) {
    for (const Value& arg : c.args) {
        if (arg.kind != ValueKind::Int) {
            Fail(c, c.name + " takes integers, got " + DescribeValue(arg));
            return false;
        }
    }
    return true;
}

bool NeedList(LibCall& c, const Value& value) {
    if (value.kind == ValueKind::List) {
        return true;
    }
    Fail(c, c.name + " needs a list, got " + DescribeValue(value));
    return false;
}

bool ListSizeOk(LibCall& c, std::size_t size) {
    if (size <= kMaxListItems) {
        return Step(c.run, size);
    }
    Fail(c, c.name + " would make a list of " + std::to_string(size) + " items; at most " +
                std::to_string(kMaxListItems));
    return false;
}

Value Overflow(LibCall& c) {
    return Fail(c, ShowValue(c.args[0]) + " " + c.name + " " + ShowValue(c.args[1]) +
                       " does not fit in 64 bits");
}

// ---- numbers

Value FloatArith(LibCall& c, double a, double b) {
    std::int32_t module = c.frame.module;
    switch (c.name[0]) {
    case '+':
        return FloatValue(a + b, module);
    case '-':
        return FloatValue(a - b, module);
    case '*':
        return FloatValue(a * b, module);
    default:
        if (b == 0.0) {
            return Fail(c, "division by zero: " + ShowValue(c.args[0]) + " / 0");
        }
        return FloatValue(a / b, module);
    }
}

Value Arith(LibCall& c) {
    if (!Count(c, 2, "(op a b)")) {
        return Value();
    }
    const Value& a = c.args[0];
    const Value& b = c.args[1];
    bool float_op = c.name == "+" || c.name == "-" || c.name == "*" || c.name == "/";
    if (float_op && IsNumber(a) && IsNumber(b) &&
        (a.kind == ValueKind::Float || b.kind == ValueKind::Float)) {
        return FloatArith(c, AsDouble(a), AsDouble(b));
    }
    if (!NeedInts(c)) {
        return Value();
    }
    std::int64_t x = a.integer;
    std::int64_t y = b.integer;
    std::int64_t out = 0;
    const std::string& op = c.name;
    if (op == "+" && __builtin_add_overflow(x, y, &out)) {
        return Overflow(c);
    }
    if (op == "-" && __builtin_sub_overflow(x, y, &out)) {
        return Overflow(c);
    }
    if (op == "*" && __builtin_mul_overflow(x, y, &out)) {
        return Overflow(c);
    }
    if ((op == "/" || op == "%") && y == 0) {
        return Fail(c, "division by zero: " + ShowValue(a) + " " + op + " 0");
    }
    if ((op == "/" || op == "%") && x == INT64_MIN && y == -1) {
        return Overflow(c);
    }
    if (op == "/") {
        out = x / y; // toward zero, like Tisple
    } else if (op == "%") {
        out = x % y; // takes the dividend's sign
    } else if (op == "&") {
        out = x & y;
    } else if (op == "|") {
        out = x | y;
    } else if (op == "^") {
        out = x ^ y;
    } else if (op == "<<" || op == ">>") {
        if (y < 0 || y > 63) {
            return Fail(c, "a shift count is 0 to 63, got " + ShowValue(b));
        }
        if (op == ">>") {
            out = x >> y; // arithmetic: integers here have no width to be logical in
        } else {
            out = static_cast<std::int64_t>(static_cast<std::uint64_t>(x) << y);
            if ((out >> y) != x) {
                return Overflow(c);
            }
        }
    }
    return IntValue(out, c.frame.module);
}

Value Compare(LibCall& c) {
    if (!Count(c, 2, "(op a b)")) {
        return Value();
    }
    const Value& a = c.args[0];
    const Value& b = c.args[1];
    std::int32_t module = c.frame.module;
    if (c.name == "=" || c.name == "!=") {
        return BoolValue(ValuesEqual(a, b) == (c.name == "="), module);
    }
    if (!IsNumber(a) || !IsNumber(b)) {
        return Fail(c, c.name + " compares numbers, got " + DescribeValue(a) + " and " +
                           DescribeValue(b));
    }
    bool less = false;
    bool equal = false;
    if (a.kind == ValueKind::Int && b.kind == ValueKind::Int) {
        less = a.integer < b.integer;
        equal = a.integer == b.integer;
    } else {
        less = AsDouble(a) < AsDouble(b);
        equal = AsDouble(a) == AsDouble(b);
    }
    if (c.name == "<") {
        return BoolValue(less, module);
    }
    if (c.name == "<=") {
        return BoolValue(less || equal, module);
    }
    if (c.name == ">") {
        return BoolValue(!less && !equal, module);
    }
    return BoolValue(!less, module);
}

Value Maths(LibCall& c) {
    if (!Count(c, 1, "(f x)")) {
        return Value();
    }
    const Value& x = c.args[0];
    if (!IsNumber(x)) {
        return Fail(c, c.name + " takes a number, got " + DescribeValue(x));
    }
    double v = AsDouble(x);
    std::int32_t module = c.frame.module;
    if (c.name == "float") {
        return FloatValue(v, module);
    }
    if (c.name == "sin") {
        return FloatValue(std::sin(v), module);
    }
    if (c.name == "cos") {
        return FloatValue(std::cos(v), module);
    }
    if (c.name == "sqrt") {
        if (v < 0) {
            return Fail(c, "sqrt of a negative number: " + ShowValue(x));
        }
        return FloatValue(std::sqrt(v), module);
    }
    if (x.kind == ValueKind::Int) {
        return x; // floor and round of an integer
    }
    // round: halves away from zero
    double whole = c.name == "floor" ? std::floor(v) : std::round(v);
    if (!std::isfinite(whole) || whole < -9.2e18 || whole > 9.2e18) {
        return Fail(c, c.name + " of " + ShowValue(x) + " is not a 64-bit integer");
    }
    return IntValue(static_cast<std::int64_t>(whole), module);
}

// ---- lists

Value Nth(LibCall& c) {
    if (!Count(c, 2, "(nth list i)") || !NeedList(c, c.args[0])) {
        return Value();
    }
    const Value& list = c.args[0];
    const Value& index = c.args[1];
    if (index.kind != ValueKind::Int) {
        return Fail(c, "nth takes an integer index, got " + DescribeValue(index));
    }
    if (index.integer < 0 || static_cast<std::uint64_t>(index.integer) >= list.items.size()) {
        return Fail(c, "nth: index " + ShowValue(index) + " is outside the " +
                           std::to_string(list.items.size()) + "-item list " + ShowValue(list));
    }
    return list.items[static_cast<std::size_t>(index.integer)];
}

Value HeadTail(LibCall& c) {
    bool head = c.name == "head";
    if (!Count(c, 1, head ? "(head list)" : "(tail list)") || !NeedList(c, c.args[0])) {
        return Value();
    }
    std::vector<Value>& items = c.args[0].items;
    if (items.empty()) {
        return Fail(c, c.name + " of an empty list");
    }
    if (head) {
        return items[0];
    }
    std::vector<Value> rest(std::make_move_iterator(items.begin() + 1),
                            std::make_move_iterator(items.end()));
    return ListValue(std::move(rest), c.frame.module);
}

Value Append(LibCall& c) {
    std::vector<Value> out;
    for (Value& arg : c.args) {
        if (!NeedList(c, arg) || !ListSizeOk(c, out.size() + arg.items.size())) {
            return Value();
        }
        for (Value& item : arg.items) {
            out.push_back(std::move(item));
        }
    }
    return ListValue(std::move(out), c.frame.module);
}

Value Map(LibCall& c) {
    if (!Count(c, 2, "(map f list)") || !NeedList(c, c.args[1])) {
        return Value();
    }
    Value fn = c.args[0];
    if (!IsCallable(c.run, fn)) {
        return Fail(c, "map needs a function first: a macro, a fn or a function's name; got " +
                           DescribeValue(fn));
    }
    std::vector<Value> out;
    out.reserve(c.args[1].items.size());
    for (Value& item : c.args[1].items) {
        std::vector<Value> one;
        one.push_back(std::move(item));
        out.push_back(CallValue(c.run, c.frame, fn, std::move(one), c.at));
        if (Failed(c.run)) {
            return Value();
        }
    }
    return ListValue(std::move(out), c.frame.module);
}

Value Range(LibCall& c) {
    if (!Count(c, 2, "(range start end)") || !NeedInts(c)) {
        return Value();
    }
    std::int64_t start = c.args[0].integer;
    std::int64_t end = c.args[1].integer;
    std::uint64_t size =
        end > start ? static_cast<std::uint64_t>(end) - static_cast<std::uint64_t>(start) : 0;
    if (!ListSizeOk(c, size)) {
        return Value();
    }
    std::vector<Value> out;
    out.reserve(size);
    for (std::int64_t i = start; i < end; i++) {
        out.push_back(IntValue(i, c.frame.module));
    }
    return ListValue(std::move(out), c.frame.module);
}

Value Repeat(LibCall& c) {
    if (!Count(c, 2, "(repeat n x)")) {
        return Value();
    }
    const Value& n = c.args[0];
    if (n.kind != ValueKind::Int || n.integer < 0) {
        return Fail(c, "repeat takes a count of 0 or more first, got " + DescribeValue(n));
    }
    if (!ListSizeOk(c, static_cast<std::size_t>(n.integer))) {
        return Value();
    }
    std::vector<Value> out(static_cast<std::size_t>(n.integer), c.args[1]);
    return ListValue(std::move(out), c.frame.module);
}

// ---- symbols and strings

bool IsNameChar(char c) {
    return c > ' ' && c != '(' && c != ')' && c != '\'' && c != '`' && c != ',' && c != ';' &&
           c != '"' && c != ':' && c != 0x7F;
}

Value MakeSymbol(LibCall& c) {
    std::string text;
    std::int32_t module = c.frame.module;
    for (std::size_t i = 0; i < c.args.size(); i++) {
        const Value& part = c.args[i];
        if (part.kind == ValueKind::Symbol || part.kind == ValueKind::String) {
            text += part.text;
        } else if (part.kind == ValueKind::Int) {
            text += std::to_string(part.integer);
        } else {
            return Fail(c,
                        "symbol joins symbols, strings and integers, got " + DescribeValue(part));
        }
        // a name made from an argument's name resolves where that argument was written
        if (i == 0 && part.kind == ValueKind::Symbol) {
            module = part.module;
        }
    }
    for (char ch : text) {
        if (!IsNameChar(ch)) {
            return Fail(c, "symbol: \"" + text + "\" is not a name");
        }
    }
    if (text.empty()) {
        return Fail(c, "symbol: the name is empty");
    }
    return SymbolValue(text, module);
}

Value Chars(LibCall& c) {
    if (!Count(c, 1, "(chars \"text\")")) {
        return Value();
    }
    const Value& text = c.args[0];
    if (text.kind != ValueKind::String) {
        return Fail(c, "chars takes a string, got " + DescribeValue(text));
    }
    if (!ListSizeOk(c, text.text.size())) {
        return Value();
    }
    std::vector<Value> out;
    for (char ch : text.text) {
        out.push_back(IntValue(static_cast<unsigned char>(ch), c.frame.module));
    }
    return ListValue(std::move(out), c.frame.module);
}

Value Len(LibCall& c) {
    if (!Count(c, 1, "(len list)")) {
        return Value();
    }
    const Value& x = c.args[0];
    if (x.kind == ValueKind::String) {
        return IntValue(static_cast<std::int64_t>(x.text.size()), c.frame.module);
    }
    if (!NeedList(c, x)) {
        return Value();
    }
    return IntValue(static_cast<std::int64_t>(x.items.size()), c.frame.module);
}

Value RaiseError(LibCall& c) {
    std::string message;
    for (const Value& part : c.args) {
        message += part.kind == ValueKind::String ? part.text : ShowValue(part);
    }
    RunFailHere(c.run, message.empty() ? "macro " + c.run.call->def->name + " failed" : message);
    return Value();
}

} // namespace

bool IsMacroLibraryName(const std::string& name) {
    for (const char* entry : kLibraryNames) {
        if (name == entry) {
            return true;
        }
    }
    return false;
}

bool IsLibraryFunction(const std::string& name) {
    for (const char* entry : kOperators) {
        if (name == entry) {
            return true;
        }
    }
    return name != "pi" && IsMacroLibraryName(name);
}

Value CallLibrary(MacroRun& run, Frame& frame, const std::string& name, std::vector<Value>& args,
                  const Node& at) {
    LibCall c{run, frame, name, args, at};
    if (name == "=" || name == "!=" || name == "<" || name == "<=" || name == ">" || name == ">=") {
        return Compare(c);
    }
    if (name == "~") {
        if (!Count(c, 1, "(~ x)") || !NeedInts(c)) {
            return Value();
        }
        return IntValue(~args[0].integer, frame.module);
    }
    if (!IsMacroLibraryName(name)) {
        return Arith(c); // + - * / % & | ^ << >>
    }
    if (name == "sin" || name == "cos" || name == "sqrt" || name == "floor" || name == "round" ||
        name == "float") {
        return Maths(c);
    }
    if (name == "list") {
        return ListValue(std::move(args), frame.module);
    }
    if (name == "len") {
        return Len(c);
    }
    if (name == "nth") {
        return Nth(c);
    }
    if (name == "head" || name == "tail") {
        return HeadTail(c);
    }
    if (name == "append") {
        return Append(c);
    }
    if (name == "map") {
        return Map(c);
    }
    if (name == "range") {
        return Range(c);
    }
    if (name == "repeat") {
        return Repeat(c);
    }
    if (name == "symbol") {
        return MakeSymbol(c);
    }
    if (name == "chars") {
        return Chars(c);
    }
    if (name == "kind") {
        if (!Count(c, 1, "(kind x)")) {
            return Value();
        }
        return SymbolValue(KindName(args[0].kind), frame.module);
    }
    return RaiseError(c);
}

} // namespace tisple
