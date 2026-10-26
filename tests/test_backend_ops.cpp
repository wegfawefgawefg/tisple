// every operator at every width on the 6502, against the target arithmetic the interpreter
// uses (check/arith.hpp): all u8/i8 pairs, and edge values plus a spread for u16/i16

#include "check/arith.hpp"
#include "test_backend.hpp"

#include <cstdio>
#include <map>

namespace tisple_tests {

namespace {

using tisple::Op;
using tisple::TypeKind;

struct OpCase {
    const char* name; // function in the generated program
    const char* form; // the operator as written
    Op op;
    bool count; // shift: b is a u8 count
};

const OpCase kOps[] = {
    {"add", "+", Op::Add, false}, {"sub", "-", Op::Sub, false},  {"mul", "*", Op::Mul, false},
    {"div", "/", Op::Div, false}, {"mod", "%", Op::Mod, false},  {"band", "&", Op::And, false},
    {"bor", "|", Op::Or, false},  {"bxor", "^", Op::Xor, false}, {"shl", "<<", Op::Shl, true},
    {"shr", ">>", Op::Shr, true}, {"lt", "<", Op::Lt, false},    {"le", "<=", Op::Le, false},
    {"gt", ">", Op::Gt, false},   {"ge", ">=", Op::Ge, false},   {"eq", "=", Op::Eq, false},
    {"ne", "!=", Op::Ne, false},
};

bool IsCompare(Op op) {
    return op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge || op == Op::Eq ||
           op == Op::Ne;
}

// three shapes per operator: the value itself, through a branch, and with b from memory
std::string ProgramFor(const char* type) {
    std::string t = type;
    std::string text = "(let gb " + t + ")\n(let gc u8)\n";
    for (const OpCase& c : kOps) {
        std::string b_type = c.count ? "u8" : t;
        std::string ret = IsCompare(c.op) ? "bool" : t;
        std::string args = "(args (a " + t + ") (b " + b_type + "))";
        text += "(fn " + std::string(c.name) + " " + args + " (returns " + ret + ") (" + c.form +
                " a b))\n";
        if (IsCompare(c.op)) {
            text += "(fn br_" + std::string(c.name) + " " + args + " (returns u8) (if (" + c.form +
                    " a b) 7 9))\n";
        }
        std::string global = c.count ? "gc" : "gb";
        text += "(fn mem_" + std::string(c.name) + " " + args + " (returns " + ret + ") (set " +
                global + " b) (" + c.form + " (id a) " + global + "))\n";
    }
    text += "(fn id (args (a " + t + ")) (returns " + t + ") a)\n";
    text += "(fn bnot (args (a " + t + ")) (returns " + t + ") (~ a))\n";
    text += "(fn to_u8 (args (a " + t + ")) (returns u8) (u8 a))\n";
    text += "(fn to_i8 (args (a " + t + ")) (returns i8) (i8 a))\n";
    text += "(fn to_u16 (args (a " + t + ")) (returns u16) (u16 a))\n";
    text += "(fn to_i16 (args (a " + t + ")) (returns i16) (i16 a))\n";
    return text;
}

std::vector<std::uint16_t> Values(TypeKind kind) {
    std::vector<std::uint16_t> values;
    if (tisple::KindBits(kind) == 8) {
        for (std::uint32_t v = 0; v < 256; v++) {
            values.push_back(static_cast<std::uint16_t>(v));
        }
        return values;
    }
    std::uint16_t edges[] = {0,      1,      2,      3,     7,      0x7F,   0x80,
                             0xFF,   0x100,  0x101,  0x1FF, 0x7FFF, 0x8000, 0x8001,
                             0xFF00, 0xFFFE, 0xFFFF, 1000,  0x55AA, 0xAA55};
    for (std::uint16_t v : edges) {
        values.push_back(v);
    }
    std::uint32_t seed = 12345;
    for (int i = 0; i < 60; i++) {
        seed = seed * 1103515245u + 12345u;
        values.push_back(static_cast<std::uint16_t>(seed >> 12));
    }
    return values;
}

struct OpRunner {
    TestResults* results = nullptr;
    const Compiled* c = nullptr;
    tisple::SimRunner sim;
    int failures = 0;
    std::uint64_t calls = 0;
    std::uint64_t cycles = 0;

    std::map<std::string, std::int32_t> ids;

    std::uint16_t Call(const std::string& fn, std::uint16_t a, std::uint16_t b, bool& ok) {
        const tisple::Program& program = c->built.program;
        auto found = ids.find(fn);
        if (found == ids.end()) {
            found = ids.emplace(fn, tisple::FindFunction(program, "t." + fn)).first;
        }
        std::int32_t id = found->second;
        tisple::SimCall r = tisple::CallCompiled(sim, program, c->code, id, {a, b}, 100000);
        calls++;
        cycles += r.cycles;
        ok = r.returned;
        const tisple::Function& f = program.functions[static_cast<std::size_t>(id)];
        std::uint32_t size = tisple::TypeOf(program, f.ret).size;
        return size == 2 ? static_cast<std::uint16_t>(r.a | (r.x << 8)) : r.a;
    }

    void Expect(const std::string& fn, std::uint16_t a, std::uint16_t b, std::uint16_t want) {
        bool ok = false;
        std::uint16_t got = Call(fn, a, b, ok);
        if (ok && got == want) {
            return;
        }
        failures++;
        if (failures <= 10) {
            std::fprintf(stderr, "%s %u %u: 6502 gives %u (%s), want %u\n", fn.c_str(), a, b, got,
                         ok ? "returned" : "did not return", want);
        }
    }
};

void TestType(TestResults& results, const char* type, TypeKind kind) {
    Compiled c = CompileText(ProgramFor(type));
    if (!c.ok) {
        std::fprintf(stderr, "ops program for %s: %s\n", type, c.errors.c_str());
        CHECK(results, c.ok);
        return;
    }
    OpRunner run;
    run.results = &results;
    run.c = &c;
    std::string error;
    CHECK(results, tisple::StartCompiled(run.sim, c.code, error));
    std::vector<std::uint16_t> values = Values(kind);
    std::uint16_t mask = tisple::KindBits(kind) == 8 ? 0xFF : 0xFFFF;
    for (const OpCase& oc : kOps) {
        for (std::uint16_t a : values) {
            std::vector<std::uint16_t> bs = values;
            if (oc.count) {
                bs.clear();
                for (std::uint16_t n = 0; n < 20; n++) {
                    bs.push_back(n);
                }
                bs.push_back(0xFF);
            }
            for (std::uint16_t b : bs) {
                if ((oc.op == Op::Div || oc.op == Op::Mod) && b == 0) {
                    continue;
                }
                bool by_zero = false;
                std::uint16_t want = tisple::EvalBinaryOp(oc.op, kind, a, b, by_zero) & mask;
                run.Expect(oc.name, a, b, want);
                // the other shapes on a spread of pairs: they share the operator code
                if ((a * 7 + b * 3) % 11 != 0) {
                    continue;
                }
                run.Expect(std::string("mem_") + oc.name, a, b, want);
                if (IsCompare(oc.op)) {
                    run.Expect(std::string("br_") + oc.name, a, b, want ? 7 : 9);
                }
            }
        }
    }
    for (std::uint16_t a : values) {
        run.Expect("bnot", a, 0, tisple::EvalUnaryOp(Op::BitNot, kind, a));
        run.Expect("to_u8", a, 0, tisple::ConvertBits(kind, TypeKind::U8, a));
        run.Expect("to_i8", a, 0, tisple::ConvertBits(kind, TypeKind::I8, a));
        run.Expect("to_u16", a, 0, tisple::ConvertBits(kind, TypeKind::U16, a));
        run.Expect("to_i16", a, 0, tisple::ConvertBits(kind, TypeKind::I16, a));
    }
    if (run.failures != 0) {
        std::fprintf(stderr, "%s: %d of %llu operator results differ\n", type, run.failures,
                     static_cast<unsigned long long>(run.calls));
    }
    CHECK(results, run.failures == 0);
}

} // namespace

void RunBackendOpTests(TestResults& results) {
    TestType(results, "u8", TypeKind::U8);
    TestType(results, "i8", TypeKind::I8);
    TestType(results, "u16", TypeKind::U16);
    TestType(results, "i16", TypeKind::I16);
}

} // namespace tisple_tests
