#include "fuzz_gen.hpp"

#include "fuzz_gen_internal.hpp"

// statements, functions and whole programs

namespace tisple_tests::fuzz {

// ---- statements
std::string Gen::Place(T& t) {
    for (int tries = 0; tries < 8; tries++) {
        t = AnyType();
        int pick = rng.Below(10);
        if (pick < 5) {
            std::vector<const VarInfo*> vars = VarsOf(t, true);
            if (!vars.empty()) {
                return vars[static_cast<std::size_t>(rng.Below(static_cast<int>(vars.size())))]
                    ->name;
            }
        } else if (pick < 8) {
            std::vector<const ArrayInfo*> arrs = ArraysOf(t, true);
            if (!arrs.empty()) {
                const ArrayInfo& a =
                    *arrs[static_cast<std::size_t>(rng.Below(static_cast<int>(arrs.size())))];
                return "(at " + a.name + " " + Index(a) + ")";
            }
        } else {
            for (int f = 0; f < 4; f++) {
                if (kFieldTypes[f] == t) {
                    return StructField(t);
                }
            }
        }
    }
    t = T::U8;
    return globals[0].name;
}

// pointers declared in scopes that ended
void Gen::DropPointers() {
    while (!ptrs.empty() && ptrs.back().depth > scopes.size()) {
        ptrs.pop_back();
    }
}

std::string Gen::Body(int depth, int count) {
    scopes.emplace_back();
    std::string text;
    for (int i = 0; i < count && budget > 0; i++) {
        text += " " + Stmt(depth);
    }
    scopes.pop_back();
    DropPointers();
    return text.empty() ? " (do)" : text;
}

// an if arm or a match clause body whose value is dropped: statements, or now and then a
// value of any type, since only a used value needs arms of one type
std::string Gen::Arm(int depth, int count, bool clause) {
    if (rng.Chance(15)) {
        return " " + NonLit(AnyType(), 2);
    }
    if (clause && rng.Chance(15)) {
        return "";
    }
    std::string body = Body(depth, count);
    if (clause || count == 1) {
        return body;
    }
    return " (do" + body + ")";
}

std::string Gen::LoopBody(int depth, bool is_while) {
    bool was_while = in_while;
    in_while = is_while;
    loops++;
    std::string body = Body(depth - 1, 1 + rng.Below(3));
    loops--;
    in_while = was_while;
    return body;
}

std::string Gen::For(int depth) {
    T t = kInts[rng.Below(4)];
    std::string i = Fresh("n");
    std::string start;
    std::string end;
    bool is_signed = t == T::I8 || t == T::I16;
    int lo = is_signed ? rng.Below(8) - 4 : rng.Below(4);
    start = std::to_string(lo);
    if (rng.Chance(50)) {
        end = std::to_string(lo + rng.Below(8));
    } else if (t == T::U8) {
        end = "(& " + NonLit(T::U8, 1) + " 7)";
    } else {
        end = std::string("(as ") + Name(t) + " (& " + NonLit(T::U8, 1) + " 7))";
    }
    if (rng.Chance(20) && t == T::U8) {
        start = "(& " + NonLit(T::U8, 1) + " 3)";
    }
    if (rng.Chance(5) && t == T::U8) {
        start = "250";
        end = "256";
    }
    bool small = true;
    if (t == T::U16) {
        static const char* const kRanges[][2] = {
            {"250", "262"}, {"250", "256"}, {"500", "512"}, {"254", "258"}, {"65533", "65536"}};
        int pick = rng.Below(9);
        if (pick < 5) {
            start = kRanges[pick][0];
            end = kRanges[pick][1];
            small = pick != 4;
        }
    }
    std::string head = "(for " + i + " " + Name(t) + " " + start + " " + end;
    std::string outer_wide = wide;
    std::string outer_ptr = wide_ptr;
    std::string prefix;
    if (t == T::U16 && small && rng.Chance(70)) {
        std::string offset = NonLit(T::U8, 1);
        wide = i;
        wide_ptr = Fresh("wp");
        prefix =
            "(do (let " + wide_ptr + " (ptr u8) (addr_of (at wb (as u16 (& " + offset + " 15))))) ";
    }
    std::string outer_counter = counter;
    if (t == T::U8) {
        counter = i;
    }
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{i, t, true});
    // the stride lets first, so they only read what is in scope before the body
    std::string strides;
    if (t == T::U8 && rng.Chance(35)) {
        bool minus_one = start == std::to_string(lo) && lo >= 1;
        strides = " " + StrideLets(i, minus_one, start != "250");
    }
    std::string body = LoopBody(depth, false);
    if (t == T::U8 && rng.Chance(60)) {
        body = " " + RoomStmt() + body;
    }
    body = strides + body;
    counter = outer_counter;
    wide = outer_wide;
    wide_ptr = outer_ptr;
    scopes.pop_back();
    DropPointers();
    return prefix + head + body + ")" + (prefix.empty() ? "" : ")");
}

std::string Gen::While(int depth) {
    std::string c = Fresh("c");
    std::string text = "(do (let " + c + " u8 0) (while (< " + c + " " +
                       std::to_string(1 + rng.Below(5)) + ") (set " + c + " (+ " + c + " 1))";
    scopes.emplace_back();
    scopes.back().push_back(VarInfo{c, T::U8, true});
    text += LoopBody(depth, true) + "))";
    scopes.pop_back();
    DropPointers();
    return text;
}

std::string Gen::Match(int depth) {
    if (rng.Chance(40)) {
        std::string text = "(match " + NonLit(T::Enum, 1);
        for (int m = 0; m < kEnumMembers; m++) {
            text += " (E.e" + std::to_string(m) + Arm(depth - 1, 1 + rng.Below(2), true) + ")";
        }
        return text + ")";
    }
    bool dense = rng.Chance(50);
    std::string text = "(match " + NonLit(T::U8, 1);
    int clauses = 2 + rng.Below(6);
    int value = rng.Below(20);
    for (int c = 0; c < clauses; c++) {
        text += " (" + std::to_string(value) + Arm(depth - 1, 1 + rng.Below(2), true) + ")";
        value += dense ? 1 : 1 + rng.Below(40);
        if (value > 255) {
            break;
        }
    }
    return text + " (_" + Arm(depth - 1, 1, true) + "))";
}

std::string Gen::PointerLet() {
    std::vector<const ArrayInfo*> arrs;
    for (const ArrayInfo& a : arrays) {
        if (a.pointers) {
            arrs.push_back(&a);
        }
    }
    if (rng.Chance(35)) {
        return RoomPointerLet();
    }
    std::string p = Fresh("p");
    std::string text;
    if (rng.Chance(30)) {
        text = "(let " + p + " (ptr S) (addr_of (at sp (& " + NonLit(T::U8, 1) + " 3))))";
        ptrs.push_back(PtrInfo{p, T::U8, true, scopes.size()});
        return text;
    }
    const ArrayInfo& a = *arrs[static_cast<std::size_t>(rng.Below(static_cast<int>(arrs.size())))];
    std::string index = "(& " + NonLit(T::U8, 1) + " " + std::to_string(a.count / 2 - 1) + ")";
    if (a.count > 256) {
        index = "(as u16 " + index + ")";
    }
    text = "(let " + p + " (ptr " + Name(a.elem) + ") (addr_of (at " + a.name + " " + index + ")))";
    ptrs.push_back(PtrInfo{p, a.elem, false, scopes.size()});
    return text;
}

std::string Gen::Store() {
    for (const PtrInfo& p : ptrs) {
        if (p.to_struct && rng.Chance(40)) {
            int f = rng.Below(4);
            if (rng.Chance(25)) {
                return "(store " + p.name + " (make S :a " + Expr(T::U8, 1) + " :b " +
                       Expr(T::I16, 1) + " :c " + Expr(T::Bool, 1) + " :d " + Expr(T::U8, 1) + "))";
            }
            return std::string("(store ") + p.name + " " + kFields[f] + " " +
                   Expr(kFieldTypes[f], 2) + ")";
        }
    }
    for (const PtrInfo& p : ptrs) {
        if (!p.to_struct && rng.Chance(60)) {
            if (fold_shapes && side.Chance(25)) {
                return "(store " + ChainedPointer(p, 2) + " " + Expr(p.elem, 2) + ")";
            }
            std::string target = rng.Chance(p.room ? 20 : 50)
                                     ? p.name
                                     : "(+ " + p.name + " " + PointerCount(p, 2) + ")";
            return "(store " + target + " " + Expr(p.elem, 2) + ")";
        }
    }
    return "";
}

std::string Gen::PlaceOf(T t) {
    std::vector<const VarInfo*> vars = VarsOf(t, true);
    std::vector<const ArrayInfo*> arrs = ArraysOf(t, true);
    if (!arrs.empty() && (vars.empty() || rng.Chance(40))) {
        const ArrayInfo& a =
            *arrs[static_cast<std::size_t>(rng.Below(static_cast<int>(arrs.size())))];
        return "(at " + a.name + " " + Index(a) + ")";
    }
    if (vars.empty()) {
        return "";
    }
    return vars[static_cast<std::size_t>(rng.Below(static_cast<int>(vars.size())))]->name;
}

std::string Gen::DivPair() {
    T t = kInts[rng.Below(4)];
    std::string x = rng.Chance(60) ? Var(t) : NonLit(t, 1);
    std::string d = rng.Chance(75) ? Divisor(t) : "(| " + Var(t) + " 1)";
    std::string first = PlaceOf(t);
    std::string second = PlaceOf(t);
    if (first.empty() || second.empty()) {
        return "(set " + Var(T::U16) + " 0)";
    }
    // now and then the first result goes into the dividend itself
    if (rng.Chance(15)) {
        for (const VarInfo* v : VarsOf(t, true)) {
            if (v->name == x) {
                first = x;
            }
        }
    }
    const char* ops[2] = {"/", "%"};
    if (rng.Chance(40)) {
        ops[0] = "%";
        ops[1] = "/";
    }
    std::string text = "(do (set " + first + " (" + ops[0] + " " + x + " " + d + "))";
    if (rng.Chance(30)) {
        scopes.emplace_back();
        text += " " + Stmt(0);
        scopes.pop_back();
        DropPointers();
    }
    return text + " (set " + second + " (" + ops[1] + " " + x + " " + d + ")))";
}

std::string Gen::StrideLets(const std::string& loop_var, bool minus_one, bool small) {
    static const char* const kStrides[] = {"1", "5", "42", "64"};
    std::string d = std::to_string(rng.Below(3));
    std::string row = "(+ " + loop_var + " " + d + ")";
    // a u16 is fine with a row that wraps; a pointer must stay inside wb
    int pick = small ? rng.Below(3) : 2;
    if ((minus_one || pick == 2) && rng.Chance(30)) {
        row = "(- " + loop_var + " 1)";
    }
    std::string base = "(addr_of (at wb " + std::to_string(rng.Below(4)) + "))";
    std::string q = Fresh("q");
    std::string value;
    switch (pick) {
    case 0:
        value = "(rowp " + base + " " + row + ")";
        break;
    case 1:
        value = "(+ " + base + " (* (as u16 " + row + ") " + kStrides[rng.Below(4)] + "))";
        break;
    default: {
        std::vector<const VarInfo*> words = VarsOf(T::U16, false);
        std::string w =
            words[static_cast<std::size_t>(rng.Below(static_cast<int>(words.size())))]->name;
        std::string text = "(let " + q + " u16 (+ (* (as u16 " + row + ") " +
                           std::to_string(1 + rng.Below(4000)) + ") " + w + "))";
        std::vector<const VarInfo*> sinks = VarsOf(T::U16, true);
        if (sinks.empty()) {
            return text;
        }
        std::string sink =
            sinks[static_cast<std::size_t>(rng.Below(static_cast<int>(sinks.size())))]->name;
        return text + " (set " + sink + " (^ " + sink + " (+ " + q + " (>> " + q + " 3))))";
    }
    }
    std::string text = "(let " + q + " (ptr u8) " + value + ")";
    text += " (store (+ " + q + " (as u16 (& " + NonLit(T::U8, 1) + " 7))) " + Expr(T::U8, 1) + ")";
    std::vector<const VarInfo*> sinks = VarsOf(T::U8, true);
    if (!sinks.empty()) {
        std::string sink =
            sinks[static_cast<std::size_t>(rng.Below(static_cast<int>(sinks.size())))]->name;
        text += " (set " + sink + " (+ " + sink + " (load " + q + ")))";
    }
    return text;
}

std::string Gen::SmallStore() {
    std::string k = std::to_string(rng.Below(4));
    std::string index = "(& " + NonLit(T::U8, 1) + " 31)";
    if (sm_big && rng.Chance(30)) {
        k = std::to_string(250 + rng.Below(6));
        index = std::to_string(rng.Below(4));
    } else if (rng.Chance(20)) {
        k = "(if " + Expr(T::Bool, 1) + " 1 0)";
    }
    switch (rng.Below(3)) {
    case 0:
        return "(set (at sm " + index + ") " + k + ")";
    case 1:
        return "(store (addr_of (at sm " + index + ")) " + k + ")";
    default:
        return "(store (+ (addr_of (at sm (& " + NonLit(T::U8, 1) + " 15))) (as u16 (& " +
               NonLit(T::U8, 1) + " 15))) " + k + ")";
    }
}

std::string Gen::RoomPointerLet() {
    // room for any byte count: the start is at most 255 into wb's 1024 bytes
    std::string p = Fresh("p");
    // now and then right at the bytes byte indices read, (at wb 0) to (at wb 3)
    std::string start =
        rng.Chance(30) ? std::to_string(rng.Below(4)) : "(as u16 " + NonLit(T::U8, 1) + ")";
    std::string text = "(let " + p + " (ptr u8) (addr_of (at wb " + start + ")))";
    ptrs.push_back(PtrInfo{p, T::U8, false, scopes.size(), true});
    return text;
}

std::string Gen::RoomStmt() {
    std::vector<const PtrInfo*> room;
    for (const PtrInfo& p : ptrs) {
        if (p.room) {
            room.push_back(&p);
        }
    }
    if (room.empty()) {
        return RoomPointerLet();
    }
    const PtrInfo& p = *room[static_cast<std::size_t>(rng.Below(static_cast<int>(room.size())))];
    std::string target = "(+ " + p.name + " " + ByteIndex(2) + ")";
    if (rng.Chance(50)) {
        return "(store " + target + " " + Expr(T::U8, 2) + ")";
    }
    std::vector<const VarInfo*> vars = VarsOf(T::U8, true);
    const std::string& v =
        vars[static_cast<std::size_t>(rng.Below(static_cast<int>(vars.size())))]->name;
    return "(set " + v + " (+ " + v + " (load " + target + ")))";
}

std::string Gen::IndexLet() {
    std::string name = Fresh("ix");
    std::string value = ByteForm();
    if (rng.Chance(30)) {
        scopes.back().push_back(VarInfo{name, T::U8, true, false});
        return "(let " + name + " u8 " + value + ")";
    }
    scopes.back().push_back(VarInfo{name, T::U16, true, true});
    return "(let " + name + " u16 (as u16 " + value + "))";
}

std::string Gen::WideStmt() {
    switch (rng.Below(3)) {
    case 0:
        return "(set (at ww " + wide + ") " + Expr(T::U16, 2) + ")";
    case 1:
        return "(set (at wb " + wide + ") " + Expr(T::U8, 2) + ")";
    default:
        return "(store (+ " + wide_ptr + " " + wide + ") " + Expr(T::U8, 2) + ")";
    }
}

std::string Gen::Stmt(int depth) {
    budget--;
    if (dead_shapes && depth > 0 && dead_rng.Chance(20)) {
        return DeadShape(depth);
    }
    if (entry_shapes && depth > 0 && entry_rng.Chance(15)) {
        return EntryLoop(depth);
    }
    if (late_shapes && late_rng.Chance(10)) {
        return LateWait();
    }
    if (loop_shapes && depth > 0 && loops_rng.Chance(12)) {
        return LoopShape(depth);
    }
    if (!wide.empty() && rng.Chance(35)) {
        return WideStmt();
    }
    int pick = rng.Below(depth > 0 ? 16 : 6);
    switch (pick) {
    case 0:
    case 1: {
        if (rng.Chance(25)) {
            return IndexLet();
        }
        if (rng.Chance(12)) {
            return DivPair();
        }
        T t = AnyType();

        std::string name = Fresh("l");
        std::string text = std::string("(let ") + name + " " + Name(t) + " " + Expr(t, 3) + ")";
        scopes.back().push_back(VarInfo{name, t, false});
        return text;
    }
    case 2:
    case 3:
    case 4: {
        if (rng.Chance(10)) {
            return SmallStore();
        }
        T t = T::U8;
        std::string place = Place(t);
        return "(set " + place + " " + Expr(t, 3) + ")";
    }
    case 5: {
        const PtrInfo* p = Pointer(T::U8);
        if (p && rng.Chance(20)) {
            return "(poke2 " + p->name + " " + Expr(T::U8, 1) + " " + Expr(T::U8, 2) + ")";
        }
        std::string store = Store();
        if (!store.empty()) {
            return store;
        }
        if (rng.Chance(50)) {
            if (race) {
                return RaceStore();
            }
            return "(io_write u8 " + IoAddress() + " " + Expr(T::U8, 2) + ")";
        }
        return PointerLet();
    }
    case 6:
    case 7:
        if (rng.Chance(50)) {
            return "(if " + Expr(T::Bool, 2) + Arm(depth - 1, 1 + rng.Below(2), false) + ")";
        }
        return "(if " + Expr(T::Bool, 2) + Arm(depth - 1, 1 + rng.Below(2), false) +
               Arm(depth - 1, 1 + rng.Below(2), false) + ")";
    case 8:
    case 9:
        return For(depth);
    case 10:
        return While(depth);
    case 11:
        return Match(depth);
    case 12: {
        if (current == 0) {
            if (race) {
                return RaceStore();
            }
            return "(io_read u8 " + IoAddress() + ")";
        }
        return CallText(fns[static_cast<std::size_t>(rng.Below(static_cast<int>(current)))], 2);
    }
    case 13:
        if (loops > 0) {
            const char* exit = rng.Chance(50) || in_while ? "(break)" : "(continue)";
            return "(if " + Expr(T::Bool, 2) + " " + exit + ")";
        }
        return PointerLet();
    case 14: {
        const FnInfo& fn = fns[current];
        std::string value = fn.has_ret ? " " + Expr(fn.ret, 2) : "";
        return "(if " + Expr(T::Bool, 2) + " (return" + value + "))";
    }
    default: {
        std::vector<const VarInfo*> vars = VarsOf(T::U16, true);
        const std::string& v =
            vars[static_cast<std::size_t>(rng.Below(static_cast<int>(vars.size())))]->name;
        return "(set " + v + " (+ " + v + " 1))";
    }
    }
}

std::string Gen::Function(std::size_t index) {
    current = index;
    const FnInfo& fn = fns[index];
    scopes.clear();
    ptrs.clear();
    scopes.emplace_back();
    std::string text = "(fn " + fn.name + " (";
    for (std::size_t p = 0; p < fn.params.size(); p++) {
        std::string name = Fresh("a");
        text += std::string(p == 0 ? "" : " ") + "(" + name + " " + Name(fn.params[p]) + ")";
        scopes.back().push_back(VarInfo{name, fn.params[p], false});
    }
    text += std::string(") ") + (fn.has_ret ? Name(fn.ret) : "void");
    if (rng.Chance(50)) {
        text += "\n    " + RoomPointerLet();
    }

    // small functions get inlined, big ones called
    budget = fn.name != entry && rng.Chance(40) ? 1 + rng.Below(2) : 6 + rng.Below(14);
    while (budget > 0) {
        text += "\n    " + Stmt(3);
    }
    if (fn.has_ret) {
        text += "\n    " + Expr(fn.ret, 3);
    }
    return text + ")\n";
}

std::string Gen::Program() {
    std::string text = "(struct S ((a u8) (b i16) (c bool) (d u8)))\n(enum E (";
    for (int m = 0; m < kEnumMembers; m++) {
        text += std::string(m == 0 ? "" : " ") + "e" + std::to_string(m);
    }
    text += "))\n";
    for (T t : {T::U8, T::I8, T::U16, T::I16, T::Bool, T::Enum}) {
        int count = 1 + rng.Below(3);
        for (int i = 0; i < count; i++) {
            std::string name = Fresh("g");
            std::string init = rng.Chance(70) ? " " + Literal(t) : "";
            if (race) {
                init = RaceStart(name, t, init);
            }
            text += std::string("(let ") + name + " " + Name(t) + init + ")\n";
            globals.push_back(VarInfo{name, t, false});
        }
    }
    static const std::uint32_t kCounts[] = {4, 8, 16, 32, 256, 512};
    int count = 3 + rng.Below(4);
    for (int i = 0; i < count; i++) {
        ArrayInfo a;
        a.name = Fresh("arr");
        a.elem = static_cast<T>(rng.Below(5));
        a.count = kCounts[rng.Below(6)];
        a.pointers = IsInt(a.elem) && a.count >= 8 && rng.Chance(40);
        std::string type =
            std::string("(array ") + Name(a.elem) + " " + std::to_string(a.count) + ")";
        std::string init;
        if (a.count <= 16 && rng.Chance(40)) {
            // a top-level array may start from a value
            init = " (make " + type;
            for (std::uint32_t k = 0; k < a.count; k++) {
                init += " " + Literal(a.elem);
            }
            init += ")";
        }
        if (race) {
            init = RaceArrayStart(a.name, a.elem, init);
        }
        text += "(let " + a.name + " " + type + init + ")\n";
        arrays.push_back(a);
    }
    // one array of pointer targets always exists
    ArrayInfo target;
    target.name = "pt";
    target.elem = T::U8;
    target.count = 16;
    target.pointers = true;
    text += "(let pt (array u8 16))\n(let wb (array u8 1024))\n(let ww (array u16 512))\n";
    text += "(let sm (array u8 32))\n";
    sm_big = rng.Chance(20);
    arrays.push_back(target);
    ArrayInfo table;
    table.name = "KT";
    table.elem = T::U16;
    table.count = 8;
    table.is_const = true;
    text += "(const KT (array u16 8) (make (array u16 8)";
    for (int i = 0; i < 8; i++) {
        text += " " + Literal(T::U16);
    }
    text += "))\n";
    arrays.push_back(table);
    text += "(let sa (array S 8))\n(let sp (array S 4))\n";
    if (race) {
        text += "(let gs S)\n" + RaceStructStart();
    } else {
        text += "(let gs S (make S :a " + Literal(T::U8) + " :b " + Literal(T::I16) + " :c " +
                Literal(T::Bool) + " :d " + Literal(T::U8) + "))\n";
    }

    // rows of wb, 37 bytes apart, for StrideLets
    text += "(fn rowp ((b (ptr u8)) (y u8)) (ptr u8) (+ b (* (as u16 y) 37)))\n";
    // small helpers taking pointers, inlined with the pointer read where it is
    text += "(fn peek2 ((p (ptr u8)) (k u8)) u8 (load (+ p (as u16 (& k 3)))))\n";
    text += "(fn poke2 ((p (ptr u8)) (k u8) (v u8)) void (store (+ p (as u16 (& k 3))) v))\n";
    int fn_count = 2 + rng.Below(4);
    for (int f = 0; f < fn_count; f++) {
        FnInfo fn;
        bool is_main = f == fn_count - 1;
        fn.name = is_main ? entry : "f" + std::to_string(f);
        int params = is_main ? 0 : rng.Below(4);
        for (int p = 0; p < params; p++) {
            fn.params.push_back(AnyType());
        }
        fn.has_ret = is_main || rng.Chance(70);
        fn.ret = AnyType();
        fns.push_back(fn);
    }
    for (std::size_t f = 0; f < fns.size(); f++) {
        text += Function(f);
    }
    if (race) {
        text += RaceTail();
    }
    return text;
}

} // namespace tisple_tests::fuzz

namespace tisple_tests {

namespace {

void SeedGen(fuzz::Gen& g, std::uint64_t seed) {
    g.rng.s = seed * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    if (g.rng.s == 0) {
        g.rng.s = 1;
    }
    for (int i = 0; i < 4; i++) {
        g.rng.Next();
    }
    g.side.s = seed * 0xD1B54A32D192ED03ull + 0x9E37ull;
    if (g.side.s == 0) {
        g.side.s = 1;
    }
    for (int i = 0; i < 4; i++) {
        g.side.Next();
    }
    // each in one program in eight
    g.fold_shapes = g.side.Chance(12);
    g.table_shapes = g.side.Chance(12);
    g.loops_rng.s = seed * 0xA24BAED4963EE407ull + 0x5851F42Dull;
    if (g.loops_rng.s == 0) {
        g.loops_rng.s = 1;
    }
    for (int i = 0; i < 4; i++) {
        g.loops_rng.Next();
    }
    // in one program in four
    g.loop_shapes = g.loops_rng.Chance(25);
    g.entry_rng.s = seed * 0x2545F4914F6CDD1Dull + 0x7F4A7C15ull;
    if (g.entry_rng.s == 0) {
        g.entry_rng.s = 1;
    }
    for (int i = 0; i < 4; i++) {
        g.entry_rng.Next();
    }
    g.late_rng.s = seed * 0x9FB21C651E98DF25ull + 0x3C6EF372ull;
    if (g.late_rng.s == 0) {
        g.late_rng.s = 1;
    }
    for (int i = 0; i < 4; i++) {
        g.late_rng.Next();
    }
    g.dead_rng.s = seed * 0xBF58476D1CE4E5B9ull + 0x94D049BBull;
    if (g.dead_rng.s == 0) {
        g.dead_rng.s = 1;
    }
    for (int i = 0; i < 4; i++) {
        g.dead_rng.Next();
    }
}

} // namespace

std::string GenerateProgram(std::uint64_t seed) {
    fuzz::Gen g;
    SeedGen(g, seed);
    return g.Program();
}

std::string GenerateEntryProgram(std::uint64_t seed) {
    fuzz::Gen g;
    SeedGen(g, seed);
    if (!g.entry_rng.Chance(kEntryPercent)) {
        return "";
    }
    g.entry_shapes = true;
    return g.Program();
}

std::string GenerateLateProgram(std::uint64_t seed) {
    fuzz::Gen g;
    SeedGen(g, seed);
    if (!g.late_rng.Chance(kLatePercent)) {
        return "";
    }
    g.late_shapes = true;
    return g.Program();
}

std::string GenerateDeadProgram(std::uint64_t seed) {
    fuzz::Gen g;
    SeedGen(g, seed);
    if (!g.dead_rng.Chance(kDeadPercent)) {
        return "";
    }
    g.dead_shapes = true;
    return g.Program();
}

std::string GenerateRaceProgram(std::uint64_t seed) {
    fuzz::Gen g;
    SeedGen(g, seed);
    g.race = true;
    g.entry = "run";
    return g.Program();
}

} // namespace tisple_tests
