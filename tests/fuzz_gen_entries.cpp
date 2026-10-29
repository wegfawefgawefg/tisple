#include "fuzz_gen_internal.hpp"

// loops entered by a jump (fuzz farm run 6): a loop body that starts with an if on a test
// that can never hold (a byte above 255, an i8 above 127) compiles to a JMP over the if's arm,
// and the branch back then goes through that JMP too, so the loop's label is reached by a jump
// from before it as well as by the fall and the branch back. What goes round the loop (a
// constant stored, a byte stored back, a load at an index the loop never moves) holds at the
// branch back but not on the way in.

namespace tisple_tests::fuzz {

namespace {

// a test that never holds, on a value of the type each one names
struct NeverTest {
    T type;
    const char* text;
};

const NeverTest kNeverTests[] = {
    {T::U8, "(> {v} 255)"},     {T::U8, "(< {v} 0)"},      {T::U8, "(not (>= {v} 0))"},
    {T::I8, "(> {v} 127)"},     {T::I8, "(< {v} -128)"},   {T::I8, "(not (<= {v} 127))"},
    {T::U16, "(> {v} 65535)"},  {T::I16, "(> {v} 32767)"}, {T::I16, "(< {v} -32768)"},
    {T::U8, "(= (& {v} 0) 1)"},
};
constexpr int kNeverCount = 10;

std::string Fill(std::string text, const std::string& v) {
    std::size_t at = text.find("{v}");
    if (at != std::string::npos) {
        text.replace(at, 3, v);
    }
    return text;
}

} // namespace

std::string Gen::NeverTrue() {
    const NeverTest& test = kNeverTests[entry_rng.Below(kNeverCount)];
    // now and then a field of gs or an element of pt, as the farm's programs had
    if (test.type == T::U8 && entry_rng.Chance(25)) {
        const char* reads[] = {"gs.a", "gs.d", "(at pt 9)"};
        return Fill(test.text, reads[entry_rng.Below(3)]);
    }
    if (test.type == T::I16 && entry_rng.Chance(25)) {
        return Fill(test.text, "gs.b");
    }
    std::vector<const VarInfo*> vars = VarsOf(test.type, false);
    std::size_t pick = static_cast<std::size_t>(entry_rng.Below(static_cast<int>(vars.size())));
    return Fill(test.text, vars[pick]->name);
}

std::string Gen::EntryCarried(int depth, const std::string& g, const std::string& k) {
    std::string byte = std::to_string(entry_rng.Below(256));
    std::string at_k = "(at wb " + k + ")";
    switch (entry_rng.Below(9)) {
    case 0:
        return "(set " + g + " " + byte + ")";
    case 1:
        return "(set " + g + " " + g + ")";
    case 2:
        // both arms the same: a test the registers decide, which the branch back may skip
        return "(set " + g + " (if (= " + g + " " + byte + ") " +
               std::to_string(entry_rng.Below(4)) + " " + std::to_string(entry_rng.Below(4)) + "))";
    case 3:
        return "(set " + at_k + " " + at_k + ")";
    case 4:
        return "(set " + g + " " + at_k + ")";
    case 5: {
        std::string inner = entry_rng.Chance(50) ? "(set " + at_k + " " + at_k + ")"
                                                 : "(set " + at_k + " " + byte + ")";
        return "(for " + Fresh("m") + " u8 0 " + std::to_string(1 + entry_rng.Below(3)) + " " +
               inner + ")";
    }
    case 6:
        return "(if " + NeverTrue() + " (set " + g + " " + byte + "))";
    case 7:
        return "(set " + g + " (if " + NeverTrue() + " " + byte + " " + g + "))";
    default:
        return Stmt(depth - 1);
    }
}

// the loop's first statement: an if on a test that never holds, as a statement or a value
std::string Gen::EntryFirst(int depth, const std::string& g, const std::string& k) {
    std::string byte = std::to_string(entry_rng.Below(256));
    switch (entry_rng.Below(6)) {
    case 0:
        return "(if " + NeverTrue() + (in_while ? " (break))" : " (continue))");
    case 1:
        return "(if " + NeverTrue() + " (break))";
    case 2:
        return "(if " + NeverTrue() + " (set " + g + " " + byte + "))";
    case 3: {
        // the arm's lets end with it
        std::string test = NeverTrue();
        scopes.emplace_back();
        std::string arm = EntryCarried(depth, g, k);
        scopes.pop_back();
        DropPointers();
        return "(if " + test + " " + arm + ")";
    }
    case 4:
        return "(set " + g + " (if " + NeverTrue() + " " + byte + " " +
               std::to_string(entry_rng.Below(256)) + "))";
    default:
        return "(set gs.a (if " + NeverTrue() + " " + byte + " " + g + "))";
    }
}

std::string Gen::EntryLoop(int depth) {
    std::vector<const VarInfo*> bytes = VarsOf(T::U8, true);
    std::size_t pick = static_cast<std::size_t>(entry_rng.Below(static_cast<int>(bytes.size())));
    std::string g = bytes[pick]->name;
    // an index the loop never moves, well inside wb
    std::string k = Fresh("k");
    std::string text = "(do (let " + k + " u16 (as u16 (& " + NonLit(T::U8, 1) + " 15)))";
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{k, T::U16, true});

    int kind = entry_rng.Below(8);
    bool was_while = in_while;
    in_while = kind == 7;
    loops++;
    std::string body = " " + EntryFirst(depth, g, k);
    int count = 1 + entry_rng.Below(3);
    for (int i = 0; i < count; i++) {
        body += " " + EntryCarried(depth, g, k);
    }
    loops--;
    in_while = was_while;
    scopes.pop_back();
    DropPointers();

    // for loops of every kind of counter and range, and a while
    static const char* const kHeads[] = {"u8 0 {v}",    "u8 0 3",   "u8 250 256", "u16 65533 65536",
                                         "u16 500 512", "i16 -3 0", "i8 -3 {v}"};
    std::string loop;
    if (kind < 7) {
        std::string count_text = "(& " + NonLit(T::U8, 1) + " 7)";
        std::string head = kHeads[kind];
        if (kind == 6) {
            count_text = "(as i8 " + count_text + ")";
        }
        loop = "(for " + Fresh("n") + " " + Fill(head, count_text) + body + ")";
    } else {
        // the step first, so the loop ends whatever the body does
        std::string c = Fresh("c");
        loop = "(do (let " + c + " u8 0) (while (< " + c + " " +
               std::to_string(1 + entry_rng.Below(5)) + ") (set " + c + " (+ " + c + " 1))" + body +
               "))";
    }
    // what went round the loop, read after it
    return text + " " + loop + " (set " + g + " (+ " + g + " (at wb " + k + "))))";
}

} // namespace tisple_tests::fuzz
