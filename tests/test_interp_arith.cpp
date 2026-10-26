#include "test_check.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

struct OpCase {
    const char* name; // Tisple spelling
    const char* fn;   // function name in the generated program
    bool compare;
    bool divide;
};

const OpCase kOps[] = {
    {"+", "add", false, false},     {"-", "sub", false, false},    {"*", "mul", false, false},
    {"&", "bit_and", false, false}, {"|", "bit_or", false, false}, {"^", "bit_xor", false, false},
    {"<<", "shl", false, false},    {">>", "shr", false, false},   {"/", "div", false, true},
    {"%", "mod", false, true},      {"=", "eq", true, false},      {"!=", "ne", true, false},
    {"<", "lt", true, false},       {"<=", "le", true, false},     {">", "gt", true, false},
    {">=", "ge", true, false},
};

// what the target computes, written out independently of check/arith.cpp
std::int32_t Reference(const std::string& fn, bool is_signed, std::int32_t bits, std::int32_t a,
                       std::int32_t b) {
    std::int32_t mask = bits == 8 ? 0xFF : 0xFFFF;
    std::int32_t half = bits == 8 ? 0x80 : 0x8000;
    std::int32_t sa = is_signed && (a & half) ? a - (mask + 1) : a;
    std::int32_t sb = is_signed && (b & half) ? b - (mask + 1) : b;
    if (fn == "add") {
        return (a + b) & mask;
    }
    if (fn == "sub") {
        return (a - b) & mask;
    }
    if (fn == "mul") {
        return static_cast<std::int32_t>((static_cast<std::int64_t>(a) * b) & mask);
    }
    if (fn == "bit_and") {
        return a & b;
    }
    if (fn == "bit_or") {
        return a | b;
    }
    if (fn == "bit_xor") {
        return a ^ b;
    }
    if (fn == "shl") {
        return b >= bits ? 0 : (a << b) & mask;
    }
    if (fn == "shr") {
        if (!is_signed) {
            return b >= bits ? 0 : a >> b;
        }
        return (sa >> (b >= bits ? bits - 1 : b)) & mask;
    }
    // by zero: what the 6502 routine gives, all quotient bits set and the dividend left over
    if (fn == "div" && b == 0) {
        return is_signed && sa < 0 ? 1 : mask;
    }
    if (fn == "mod" && b == 0) {
        return a;
    }
    if (fn == "div") {
        return (is_signed ? sa / sb : a / b) & mask;
    }
    if (fn == "mod") {
        return (is_signed ? sa % sb : a % b) & mask;
    }
    std::int32_t x = is_signed ? sa : a;
    std::int32_t y = is_signed ? sb : b;
    if (fn == "eq") {
        return x == y;
    }
    if (fn == "ne") {
        return x != y;
    }
    if (fn == "lt") {
        return x < y;
    }
    if (fn == "le") {
        return x <= y;
    }
    if (fn == "gt") {
        return x > y;
    }
    return x >= y;
}

// every (a, b) at 8 bits: each function loops b over all values into a table
void ExhaustiveByte(TestResults& results, const std::string& type) {
    bool is_signed = type == "i8";
    std::string range = is_signed ? "-128 128" : "0 256";
    std::string text = "(let out (array " + type + " 256))\n(let flags (array bool 256))\n";
    for (const OpCase& op : kOps) {
        std::string table = op.compare ? "flags" : "out";
        std::string body = "(set (at " + table + " (as u8 b)) (" + op.name + " a b))";
        // a shift count is always u8
        bool shift = std::string(op.fn) == "shl" || std::string(op.fn) == "shr";
        std::string count = shift ? "u8 0 256" : type + " " + range;
        text += "(fn " + std::string(op.fn) + " ((a " + type + ")) void (for b " + count + " " +
                body + "))\n";
    }
    Built built = BuildText(text);
    CHECK(results, built.ok);
    if (!built.ok) {
        std::fprintf(stderr, "%s", ErrorText(built).c_str());
        return;
    }
    tisple::Interp in;
    StartInterp(in, built);
    in.max_steps = 100000000;
    std::uint16_t out = built.layout.vars[tisple::FindVar(built.program, "t.out")].address;
    std::uint16_t flags = built.layout.vars[tisple::FindVar(built.program, "t.flags")].address;
    for (const OpCase& op : kOps) {
        int wrong = 0;
        for (std::int32_t a = 0; a < 256; a++) {
            CallOk(results, in, "t." + std::string(op.fn), {static_cast<std::uint16_t>(a)});
            for (std::int32_t b = 0; b < 256; b++) {
                std::int32_t want = Reference(op.fn, is_signed, 8, a, b);
                std::int32_t got = op.compare ? in.memory[flags + b] : in.memory[out + b];
                if (got != want && wrong++ < 3) {
                    std::fprintf(stderr, "%s %s: %d %s %d gives %d, want %d\n", type.c_str(), op.fn,
                                 a, op.name, b, got, want);
                }
            }
        }
        CHECK(results, wrong == 0);
    }
}

// 16 bits: edge values, both ways round
void EdgeWords(TestResults& results, const std::string& type) {
    bool is_signed = type == "i16";
    const std::int32_t edges[] = {0,      1,      2,      7,      8,      15,     16,
                                  17,     0x7F,   0x80,   0xFF,   0x100,  0x1234, 0x7FFF,
                                  0x8000, 0x8001, 0xFFFE, 0xFFFF, 0xABCD, 0x00FF};
    std::string text;
    for (const OpCase& op : kOps) {
        std::string ret = op.compare ? "bool" : type;
        bool shift = std::string(op.fn) == "shl" || std::string(op.fn) == "shr";
        text += "(fn " + std::string(op.fn) + " ((a " + type + ") (b " +
                (shift ? std::string("u8") : type) + ")) " + ret + " (" + op.name + " a b))\n";
    }
    Built built = BuildText(text);
    CHECK(results, built.ok);
    tisple::Interp in;
    StartInterp(in, built);
    for (const OpCase& op : kOps) {
        int wrong = 0;
        for (std::int32_t a : edges) {
            for (std::int32_t b : edges) {
                bool shift = std::string(op.fn) == "shl" || std::string(op.fn) == "shr";
                if (shift && b > 0xFF) {
                    continue;
                }
                std::int32_t want = Reference(op.fn, is_signed, 16, a, b);
                std::int32_t got =
                    CallOk(results, in, "t." + std::string(op.fn),
                           {static_cast<std::uint16_t>(a), static_cast<std::uint16_t>(b)});
                if (got != want && wrong++ < 3) {
                    std::fprintf(stderr, "%s %s: $%04X %s $%04X gives $%04X, want $%04X\n",
                                 type.c_str(), op.fn, a, op.name, b, got, want);
                }
            }
        }
        CHECK(results, wrong == 0);
    }
}

void Conversions(TestResults& results) {
    Built built = BuildText("(fn u8_to_i16 ((x u8)) i16 (as i16 x))\n"
                            "(fn i8_to_u16 ((x i8)) u16 (as u16 x))\n"
                            "(fn i8_to_i16 ((x i8)) i16 (as i16 x))\n"
                            "(fn u16_to_u8 ((x u16)) u8 (as u8 x))\n"
                            "(fn i16_to_i8 ((x i16)) i8 (as i8 x))\n"
                            "(fn not_u8 ((x u8)) u8 (~ x))\n"
                            "(fn not_i16 ((x i16)) i16 (~ x))");
    CHECK(results, built.ok);
    tisple::Interp in;
    StartInterp(in, built);
    int wrong = 0;
    for (std::int32_t x = 0; x < 256; x++) {
        std::uint16_t v = static_cast<std::uint16_t>(x);
        std::uint16_t sign_extended = static_cast<std::uint16_t>(x >= 0x80 ? x | 0xFF00 : x);
        wrong += CallOk(results, in, "t.u8_to_i16", {v}) != v;
        wrong += CallOk(results, in, "t.i8_to_u16", {v}) != sign_extended;
        wrong += CallOk(results, in, "t.i8_to_i16", {v}) != sign_extended;
        wrong += CallOk(results, in, "t.not_u8", {v}) != (~x & 0xFF);
        std::uint16_t word = static_cast<std::uint16_t>(x * 0x0101 ^ 0x5A00);
        wrong += CallOk(results, in, "t.u16_to_u8", {word}) != (word & 0xFF);
        wrong += CallOk(results, in, "t.i16_to_i8", {word}) != (word & 0xFF);
        wrong += CallOk(results, in, "t.not_i16", {word}) != (~word & 0xFFFF);
    }
    CHECK(results, wrong == 0);
}

} // namespace

void RunInterpArithTests(TestResults& results) {
    ExhaustiveByte(results, "u8");
    ExhaustiveByte(results, "i8");
    EdgeWords(results, "u16");
    EdgeWords(results, "i16");
    Conversions(results);
}

} // namespace tisple_tests
