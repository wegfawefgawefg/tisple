#include "fuzz_gen_internal.hpp"

// what the mid-level passes (src/opt) remove, and what they must keep: lets nobody reads,
// stores overwritten before a read (with reads, calls and exits between, or not), (set x x),
// statements with no effect, and loops with no effect outside themselves, provably ending
// or ending in ways the passes cannot prove, next to loops whose results are read after.

namespace tisple_tests::fuzz {

namespace {

const T kDeadTypes[] = {T::U8, T::I8, T::U16, T::I16};

} // namespace

T Gen::DeadInt() {
    return kDeadTypes[dead_rng.Below(4)];
}

// a writable variable of type t, named now (VarsOf's pointers move as scopes grow)
std::string Gen::DeadTarget(T t) {
    std::vector<const VarInfo*> vars = VarsOf(t, true);
    return vars[static_cast<std::size_t>(dead_rng.Below(static_cast<int>(vars.size())))]->name;
}

// a counting loop over a fresh u8, i8 or u16 c, as (do (let c ...) (while ...)); most end by
// a counter the passes can follow, some end in ways they cannot
std::string Gen::CountingWhile(int depth) {
    std::string c = Fresh("c");
    T type = T::U8;
    std::string k = std::to_string(1 + dead_rng.Below(40));
    std::string s = std::to_string(1 + dead_rng.Below(4));
    std::string lets = "(let " + c + " u8 0)";
    std::string cond = "(< " + c + " " + k + ")";
    std::string step = "(set " + c + " (+ " + c + " " + s + "))";
    switch (dead_rng.Below(9)) {
    case 0:
        break;
    case 1: {
        // down to a bound the step cannot wrap past
        int bound = dead_rng.Below(20) + 3;
        lets = "(let " + c + " u8 " + std::to_string(bound + dead_rng.Below(60)) + ")";
        cond = "(> " + c + " " + std::to_string(bound) + ")";
        step = "(set " + c + " (- " + c + " " + std::to_string(1 + dead_rng.Below(4)) + "))";
        break;
    }
    case 2:
        type = T::I8;
        lets = "(let " + c + " i8 " + std::to_string(dead_rng.Below(10) - 8) + ")";
        cond = "(< " + c + " " + std::to_string(dead_rng.Below(20) - 4) + ")";
        step = "(set " + c + " (+ " + c + " 1))";
        break;
    case 3: {
        static const int kBases[] = {0, 250, 500, 65000};
        int base = kBases[dead_rng.Below(4)];
        type = T::U16;
        lets = "(let " + c + " u16 " + std::to_string(base) + ")";
        cond = "(< " + c + " " + std::to_string(base + 1 + dead_rng.Below(30)) + ")";
        break;
    }
    case 4: {
        // up or down by one to a value it reaches
        int end = 40 + dead_rng.Below(170);
        bool up = dead_rng.Chance(50);
        int start = up ? end - dead_rng.Below(30) : end + dead_rng.Below(30);
        lets = "(let " + c + " u8 " + std::to_string(start) + ")";
        cond = "(!= " + c + " " + std::to_string(end) + ")";
        step = "(set " + c + (up ? " (+ " : " (- ") + c + " 1))";
        break;
    }
    case 5:
        // ends (255 is a multiple of 3), but 254 + 3 would wrap: not provable
        cond = "(< " + c + " 255)";
        step = "(set " + c + " (+ " + c + " 3))";
        break;
    case 6:
        cond = "true";
        step += " (if (>= " + c + " " + k + ") (break))";
        break;
    case 7: {
        // a bound in a local
        std::string bound = Fresh("k");
        lets = "(let " + bound + " u8 (& " + NonLit(T::U8, 1) + " 15)) " + lets;
        cond = "(< " + c + " " + bound + ")";
        step = "(set " + c + " (+ " + c + " 1))";
        break;
    }
    default:
        cond = "(and " + cond + " " + BoolExpr(1) + ")";
        break;
    }
    // the body: dead lets, a nested loop, now and then a statement with an effect
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{c, type, true});
    bool was_while = in_while;
    in_while = true; // no continue: it could skip a step at the end
    loops++;
    std::string body;
    int count = dead_rng.Below(3);
    for (int i = 0; i < count; i++) {
        body += " " + DeadLoopItem(depth - 1);
    }
    loops--;
    in_while = was_while;
    scopes.pop_back();
    DropPointers();
    std::string loop = dead_rng.Chance(70) ? "(while " + cond + " " + step + body + ")"
                                           : "(while " + cond + body + " " + step + ")";
    std::string after;
    if (dead_rng.Chance(15)) {
        // the counter read after the loop: it stays
        std::string g = DeadTarget(T::U8);
        std::string read = type == T::U8 ? c : "(as u8 " + c + ")";
        after = " (set " + g + " (+ " + g + " " + read + "))";
    }
    return "(do " + lets + " " + loop + after + ")";
}

// one statement of a dead loop's body
std::string Gen::DeadLoopItem(int depth) {
    int pick = dead_rng.Below(depth > 0 ? 7 : 5);
    switch (pick) {
    case 0:
    case 1: {
        T t = DeadInt();
        return "(let " + Fresh("d") + " " + Name(t) + " " + Expr(t, 2) + ")";
    }
    case 2: {
        // a local of the loop, set and read only inside it
        T t = DeadInt();
        std::string v = Fresh("v");
        return "(do (let " + v + " " + Name(t) + " " + Expr(t, 1) + ") (set " + v + " (+ " + v +
               " " + Expr(t, 1) + ")))";
    }
    case 3:
        // an effect: the loop stays
        return dead_rng.Chance(40) ? Stmt(depth) : "(do)";
    case 4:
        return NonLit(DeadInt(), 2);
    default:
        return dead_rng.Chance(50) ? CountingWhile(depth) : DeadFor(depth);
    }
}

// a for whose body is dead work, nested now and then
std::string Gen::DeadFor(int depth) {
    static const char* const kHeads[] = {"u8 0 {k}",    "u8 250 256", "i16 -3 {k}",
                                         "u16 250 262", "i8 -4 3",    "u8 (& {e} 7) 6"};
    std::string head = kHeads[dead_rng.Below(6)];
    std::string k = std::to_string(dead_rng.Below(9));
    if (loops > 0 && dead_rng.Chance(15)) {
        // a break in the range leaves the loop around this one
        k = "(do (if " + BoolExpr(1) + " (break)) " + k + ")";
    }
    std::size_t at = head.find("{k}");
    if (at != std::string::npos) {
        head.replace(at, 3, k);
    }
    at = head.find("{e}");
    if (at != std::string::npos) {
        head.replace(at, 3, NonLit(T::U8, 1));
    }
    std::string n = Fresh("n");
    T t = head[0] == 'i' ? (head[1] == '8' ? T::I8 : T::I16) : head[1] == '8' ? T::U8 : T::U16;
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{n, t, true});
    bool was_while = in_while;
    in_while = false;
    loops++;
    std::string body;
    int count = 1 + dead_rng.Below(3);
    for (int i = 0; i < count; i++) {
        body += " " + DeadLoopItem(depth - 1);
    }
    loops--;
    in_while = was_while;
    scopes.pop_back();
    DropPointers();
    return "(for " + n + " " + head + body + ")";
}

std::string Gen::DeadShape(int depth) {
    switch (dead_rng.Below(12)) {
    case 0: {
        // a let nobody reads: its value pure, a division, or a call
        T t = DeadInt();
        return "(let " + Fresh("d") + " " + Name(t) + " " + Expr(t, 3) + ")";
    }
    case 1: {
        // a local overwritten before it is read, maybe with a statement between that reads it
        T t = DeadInt();
        std::string x = Fresh("x");
        std::string text = "(do (let " + x + " " + Name(t) + " " + Expr(t, 2) + ") (set " + x +
                           " " + Expr(t, 2) + ")";
        scopes.emplace_back();
        scopes.back().push_back(VarInfo{x, t, false});
        if (dead_rng.Chance(50)) {
            text += " " + Stmt(depth - 1);
        }
        text += " (set " + x + " " + Expr(t, 2) + ")";
        std::string g = DeadTarget(t);
        text += " (set " + g + " (+ " + g + " " + x + ")))";
        scopes.pop_back();
        DropPointers();
        return text;
    }
    case 2:
    case 3: {
        // a store overwritten later: a read, a call, an exit or nothing between
        T t = DeadInt();
        std::string g = DeadTarget(t);
        std::string text = "(do (set " + g + " " + Expr(t, 2) + ")";
        switch (dead_rng.Below(5)) {
        case 0:
            // its lets end with the do
            scopes.emplace_back();
            text += " " + Stmt(depth - 1);
            scopes.pop_back();
            DropPointers();
            break;
        case 1:
            if (current > 0) {
                text += " " +
                        CallText(
                            fns[static_cast<std::size_t>(rng.Below(static_cast<int>(current)))], 2);
            }
            break;
        case 2: {
            const FnInfo& fn = fns[current];
            std::string value = fn.has_ret ? " " + Expr(fn.ret, 1) : "";
            std::string exit =
                loops > 0 && dead_rng.Chance(50) ? "(break)" : "(return" + value + ")";
            text += " (if " + BoolExpr(1) + " " + exit + ")";
            break;
        }
        case 3: {
            std::string h = DeadTarget(t);
            text += " (set " + h + " (^ " + h + " " + g + "))";
            break;
        }
        default:
            break;
        }
        return text + " (set " + g + " " + Expr(t, 2) + "))";
    }
    case 4: {
        // the same place written back
        T t = T::U8;
        std::string place = Place(t);
        return "(set " + place + " " + place + ")";
    }
    case 5:
        // a value nobody uses, or a call
        if (current > 0 && dead_rng.Chance(40)) {
            return CallText(fns[static_cast<std::size_t>(rng.Below(static_cast<int>(current)))], 2);
        }
        return NonLit(DeadInt(), 2);
    case 6: {
        // a loop whose result is read after it: it stays
        T t = DeadInt();
        std::string s = Fresh("s");
        std::string text = "(do (let " + s + " " + Name(t) + " " + Literal(t) + ") ";
        scopes.emplace_back();
        scopes.back().push_back(VarInfo{s, t, false});
        std::string add = "(set " + s + " (+ " + s + " " + Expr(t, 1) + "))";
        std::string loop;
        if (dead_rng.Chance(50)) {
            // added to at the end of each pass
            loop = DeadFor(depth);
            loop.insert(loop.rfind(')'), " " + add);
        } else {
            std::string c = Fresh("c");
            std::string x = Fresh("x");
            // and what the pass before left in x
            loop = "(do (let " + c + " u8 0) (let " + x + " " + Name(t) + " " + Literal(t) +
                   ") (while (< " + c + " " + std::to_string(1 + dead_rng.Below(6)) + ") (set " +
                   c + " (+ " + c + " 1)) " + add + " (set " + s + " (+ " + s + " " + x +
                   ")) (set " + x + " " + Expr(t, 1) + ")))";
        }
        scopes.pop_back();
        DropPointers();
        std::string g = DeadTarget(t);
        return text + loop + " (set " + g + " (^ " + g + " " + s + ")))";
    }
    case 7:
    case 8:
    case 9:
        return DeadFor(depth);
    default:
        return CountingWhile(depth);
    }
}

} // namespace tisple_tests::fuzz
