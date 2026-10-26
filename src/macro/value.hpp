#pragma once

// Values of macro code: what a macro body computes with while the compiler runs.
// Forms are values too (symbols, integers, strings, keywords and lists of them), so a macro
// takes its arguments apart and builds new forms with the same operations.
//
// - Integers are 64-bit signed; an operation that overflows is an error, never a wrap.
// - Floats, strings and functions exist only at compile time: none can be pasted into code.
// - A form from a macro argument remembers where it was written (file, span, expansion) and
//   keeps that in the output, so errors in it point at it. Everything a macro makes gets the
//   call's span instead, like template forms always did.

#include "check/diag.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tisple {

enum class ValueKind : std::uint8_t {
    Void,    // what let, set and loops give; never pasted into code
    Int,     // 64-bit signed
    Float,   // compile time only: round it before it goes into code
    Bool,    // true and false; pasted as the symbols
    Symbol,  // a name; also a function value, by its name
    Keyword, // :x
    String,  // compile time only
    List,
    Fn, // (fn (a b) body ...) evaluated in macro code
};

struct FnValue;

struct Value {
    ValueKind kind = ValueKind::Void;
    std::int64_t integer = 0;         // Int; Bool is 0 or 1
    double real = 0.0;                // Float
    std::string text;                 // Symbol, Keyword (":x"); String: its bytes
    std::string spelling;             // Int and String as written; empty: printed plainly
    std::vector<Value> items;         // List
    QuoteMark mark = QuoteMark::None; // List: 'x and friends, so forms print back the same
    std::shared_ptr<const FnValue> fn;
    // where a form from a macro argument was written (see the top of this file)
    bool written = false;
    std::uint32_t file = 0;
    Span span;
    std::uint32_t expansion = 0;
    std::int32_t module = -1; // where names in it resolve
};

struct Binding {
    std::string name;
    Value value;
};

// a fn made in macro code: its body runs with the values it saw when it was made
struct FnValue {
    std::vector<std::string> params;
    std::vector<Node> body;
    std::vector<Binding> captured;
    std::int32_t module = -1;
    std::uint32_t file = 0;
};

Value IntValue(std::int64_t integer, std::int32_t module);
Value FloatValue(double real, std::int32_t module);
Value BoolValue(bool truth, std::int32_t module);
Value SymbolValue(const std::string& text, std::int32_t module);
Value ListValue(std::vector<Value> items, std::int32_t module);

// a form as a value: true and false become Bool, everything else keeps its kind.
// written: the form came from a macro argument (see the top of this file)
Value NodeToValue(const Node& node, bool written, std::uint32_t file, std::int32_t module);

// how the forms a macro made are stamped: the call's span, and an expansion whose module is
// where the names in them resolve
struct Stamp {
    std::string macro; // the macro the code called
    Loc call;          // that call
    std::uint32_t expansion = 0;
    std::int32_t module = -1;
    Sources* sources = nullptr; // gets one more Expansion per other module names resolve in
    std::vector<std::pair<std::int32_t, std::uint32_t>> others; // module, expansion
};

// a value as a form to paste into code; false with why when it can't be one
bool ValueToNode(const Value& value, Stamp& stamp, Node& out, std::string& why);

// the value as macro code would write it, for error messages: 5, 'a, (1 2), "hi"
std::string ShowValue(const Value& value);
// "the symbol a", "the list (1 2)", "5": what a message says it got
std::string DescribeValue(const Value& value);
const char* KindName(ValueKind kind);

bool ValuesEqual(const Value& a, const Value& b);

} // namespace tisple
