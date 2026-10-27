#include "race/race_c.hpp"

#include "check/arith.hpp"
#include "race/race_c_internal.hpp"

#include <cstdio>

// statements, functions and the whole file (race_c.hpp)

namespace tisple::racec {

std::string Writer::Block(ExprId id) {
    indent++;
    std::string text = Stmt(id);
    indent--;
    return text;
}

std::string Writer::StoreStruct(const std::string& target, TypeId type, ExprId value) {
    const tisple::Expr& v = At(value);
    if (v.kind != ExprKind::StructLit) {
        Fail("a struct copy other than from make");
        return "";
    }
    // the whole value is read before any of it is written
    const StructDecl& sd = program->structs[static_cast<std::size_t>(TypeOf(*program, type).decl)];
    std::vector<CValue> fields;
    std::string text;
    for (std::uint32_t i = 0; i < v.count; i++) {
        ExprId field = program->lists[v.first + i];
        CValue f = Stabilize(Expr(field), At(field).type);
        text += f.pre;
        fields.push_back(f);
    }
    for (std::size_t i = 0; i < fields.size(); i++) {
        text += Pad() + target + "." + sd.fields[i].name + " = " + fields[i].text + ";\n";
    }
    return text;
}

std::string Writer::Assign(ExprId place, TypeId type, ExprId value) {
    if (Kind(type) == TypeKind::Struct) {
        CValue target = Place(place, true);
        return target.pre + StoreStruct(target.text, type, value);
    }
    if (At(value).kind == ExprKind::Call) {
        CValue call = Call(At(value), false);
        CValue target = Place(place, true);
        return target.pre + call.pre + Pad() + target.text + " = " + call.text + ";\n";
    }
    CValue v = Expr(value);
    CValue target = Place(place, !v.pre.empty());
    return target.pre + v.pre + Pad() + target.text + " = " + v.text + ";\n";
}

std::string Writer::StoreStmt(const tisple::Expr& e) {
    TypeId elem = TypeOf(*program, At(e.a).type).elem;
    if (Kind(elem) == TypeKind::Struct) {
        CValue pointer = Stabilize(Expr(e.a), At(e.a).type);
        return pointer.pre + StoreStruct("(*" + pointer.text + ")", elem, e.b);
    }
    // the pointer first, then the value: (store (+ p n) v) is p[n] = v
    CValue v = Expr(e.b);
    const tisple::Expr& p = At(e.a);
    CValue target;
    if (p.kind == ExprKind::PtrAdd) {
        CValue base = Expr(p.a);
        CValue count = Expr(p.b);
        Order(base, At(p.a).type, count);
        if (!v.pre.empty()) {
            base = Stabilize(base, At(p.a).type);
            count = Stabilize(count, At(p.b).type);
        }
        target.pre = base.pre + count.pre;
        target.text = PointerAt(base.text, count.text);
    } else {
        CValue pointer = Expr(e.a);
        if (!v.pre.empty()) {
            pointer = Stabilize(pointer, At(e.a).type);
        }
        target.pre = pointer.pre;
        target.text = "*" + pointer.text;
    }
    return target.pre + v.pre + Pad() + target.text + " = " + v.text + ";\n";
}

std::string Writer::StoreFieldStmt(const tisple::Expr& e) {
    if (Kind(e.ref) == TypeKind::Struct) {
        Fail("a struct field of a struct");
        return "";
    }
    CValue pointer = Expr(e.a);
    CValue v = Expr(e.b);
    Order(pointer, At(e.a).type, v);
    TypeId elem = TypeOf(*program, At(e.a).type).elem;
    std::string target = pointer.text + "->" + FieldName(*program, elem, e.value);
    return pointer.pre + v.pre + Pad() + target + " = " + v.text + ";\n";
}

std::string Writer::IfStmt(const tisple::Expr& e) {
    CValue cond = Expr(e.a);
    std::string text = cond.pre + Pad() + "if (" + cond.text + ") {\n" + Block(e.b) + Pad() + "}";
    if (e.c >= 0) {
        text += " else {\n" + Block(e.c) + Pad() + "}";
    }
    return text + "\n";
}

std::string Writer::LoopEnd(const LoopCtx& loop) {
    if (!loop.used) {
        return "";
    }
    return Pad() + "brk" + std::to_string(loop.label) + ":;\n";
}

std::string Writer::WhileStmt(const tisple::Expr& e) {
    loops.push_back(LoopCtx{++labels, switches, false});
    indent++;
    CValue cond = Expr(e.a);
    std::string body = Stmt(e.b);
    indent--;
    std::string text;
    if (cond.pre.empty()) {
        text = Pad() + "while (" + cond.text + ") {\n" + body + Pad() + "}\n";
    } else {
        text = Pad() + "for (;;) {\n" + cond.pre + Pad() + "    if (!" + cond.text + ") {\n" +
               Pad() + "        break;\n" + Pad() + "    }\n" + body + Pad() + "}\n";
    }
    LoopCtx loop = loops.back();
    loops.pop_back();
    return text + LoopEnd(loop);
}

std::string Writer::ForStmt(const tisple::Expr& e) {
    const std::string& var = var_names[static_cast<std::size_t>(e.ref)];
    TypeId type = program->vars[static_cast<std::size_t>(e.ref)].type;
    CValue start = Expr(e.a);
    std::string text;
    if (e.b >= 0) {
        // the end is worked out once, after the start
        CValue end = Expr(e.b);
        Order(start, At(e.a).type, end);
        if (At(e.b).kind != ExprKind::Literal) {
            end.stable = false;
            end = Stabilize(end, type);
        }
        loops.push_back(LoopCtx{++labels, switches, false});
        std::string body = Block(e.c);
        text = start.pre + end.pre + Pad() + "for (" + var + " = " + start.text + "; " + var +
               " < " + end.text + "; " + var + "++) {\n" + body + Pad() + "}\n";
    } else {
        // through the type's largest value, which a < test never passes
        TypeKind kind = Kind(type);
        std::uint32_t max = KindBits(kind) == 8 ? 0xFF : 0xFFFF;
        if (IsSignedKind(kind)) {
            max >>= 1;
        }
        loops.push_back(LoopCtx{++labels, switches, false});
        std::string body = Block(e.c);
        text = start.pre + Pad() + var + " = " + start.text + ";\n" + Pad() + "do {\n" + body +
               Pad() + "} while (" + var + "++ != " + Literal(type, max) + ");\n";
    }
    LoopCtx loop = loops.back();
    loops.pop_back();
    return text + LoopEnd(loop);
}

std::string Writer::MatchStmt(const tisple::Expr& e, const std::string& result) {
    CValue scrutinee = Expr(e.a);
    TypeId scrutinee_type = At(e.a).type;
    std::string text = scrutinee.pre;
    bool wildcard = false;
    for (std::uint32_t i = 0; i < e.count; i++) {
        wildcard = wildcard || program->clauses[e.first + i].wildcard;
    }
    if (!result.empty() && !wildcard) {
        text += Pad() + result + " = 0;\n";
    }
    text += Pad() + "switch (" + scrutinee.text + ") {\n";
    switches++;
    std::vector<std::uint16_t> seen;
    for (std::uint32_t i = 0; i < e.count; i++) {
        const Clause& clause = program->clauses[e.first + i];
        if (!clause.wildcard) {
            bool repeat = false;
            for (std::uint16_t v : seen) {
                repeat = repeat || v == clause.value;
            }
            if (repeat) {
                continue; // the first clause for a value is the one that runs
            }
            seen.push_back(clause.value);
            text += Pad() + "case " + Literal(scrutinee_type, clause.value) + ":\n";
        } else {
            text += Pad() + "default:\n";
        }
        indent++;
        if (clause.body >= 0 && result.empty()) {
            text += Stmt(clause.body);
        } else if (clause.body >= 0) {
            CValue v = Expr(clause.body);
            text += v.pre + Pad() + result + " = " + v.text + ";\n";
        } else if (!result.empty()) {
            text += Pad() + result + " = 0;\n";
        }
        text += Pad() + "break;\n";
        indent--;
        if (clause.wildcard) {
            break; // clauses after _ never run
        }
    }
    switches--;
    return text + Pad() + "}\n";
}

std::string Writer::Stmt(ExprId id) {
    const tisple::Expr& e = At(id);
    switch (e.kind) {
    case ExprKind::Let: {
        TypeId type = program->vars[static_cast<std::size_t>(e.ref)].type;
        const std::string& name = var_names[static_cast<std::size_t>(e.ref)];
        if (Kind(type) == TypeKind::Struct) {
            return StoreStruct(name, type, e.a);
        }
        if (At(e.a).kind == ExprKind::Call) {
            CValue call = Call(At(e.a), false);
            return call.pre + Pad() + name + " = " + call.text + ";\n";
        }
        CValue v = Expr(e.a);
        return v.pre + Pad() + name + " = " + v.text + ";\n";
    }
    case ExprKind::Set:
        return Assign(e.a, At(e.a).type, e.b);
    case ExprKind::Store:
        return StoreStmt(e);
    case ExprKind::StoreField:
        return StoreFieldStmt(e);
    case ExprKind::If:
        return IfStmt(e);
    case ExprKind::Do: {
        std::string text;
        for (std::uint32_t i = 0; i < e.count; i++) {
            text += Stmt(program->lists[e.first + i]);
        }
        return text;
    }
    case ExprKind::While:
        return WhileStmt(e);
    case ExprKind::For:
        return ForStmt(e);
    case ExprKind::Match:
        return MatchStmt(e, "");
    case ExprKind::Return: {
        bool is_main = function == program->main;
        if (e.a < 0) {
            return Pad() + (is_main ? "return 0;\n" : "return;\n");
        }
        CValue v = Expr(e.a);
        return v.pre + Pad() + "return " + v.text + ";\n";
    }
    case ExprKind::Break: {
        if (loops.empty()) {
            Fail("break outside a loop");
            return "";
        }
        LoopCtx& loop = loops.back();
        if (switches > loop.switches) {
            loop.used = true;
            return Pad() + "goto brk" + std::to_string(loop.label) + ";\n";
        }
        return Pad() + "break;\n";
    }
    case ExprKind::Continue:
        return Pad() + "continue;\n";
    case ExprKind::Call: {
        CValue call = Call(e, false);
        return call.pre + Pad() + call.text + ";\n";
    }
    default: {
        // a value nobody reads: only what it does first is kept
        CValue v = Expr(id);
        return v.pre;
    }
    }
}

std::string Writer::Params(std::int32_t index) const {
    const tisple::Function& fn = program->functions[static_cast<std::size_t>(index)];
    std::string text;
    for (std::int32_t param : fn.params) {
        TypeId type = program->vars[static_cast<std::size_t>(param)].type;
        text += std::string(text.empty() ? "" : ", ") +
                Declare(type, var_names[static_cast<std::size_t>(param)]);
    }
    return text;
}

std::string Writer::Prototype(std::int32_t index, const std::string& params) const {
    const tisple::Function& fn = program->functions[static_cast<std::size_t>(index)];
    if (index == program->main) {
        return "int main(void)";
    }
    // the race's clock starts when init returns and stops at the checksum's entry: neither
    // may be inlined
    bool marker = fn.name == "t.checksum" || fn.name == "t.init";
    std::string linkage = marker ? "RACE_NOINLINE " : "RACE_STATIC ";
    return linkage + CType(fn.ret) + " " + fn_names[static_cast<std::size_t>(index)] + "(" +
           (params.empty() ? "void" : params) + ")";
}

std::string Writer::Function(std::int32_t index) {
    const tisple::Function& fn = program->functions[static_cast<std::size_t>(index)];
    function = index;
    decls.clear();
    temps = 0;
    labels = 0;
    indent = 1;
    switches = 0;
    loops.clear();
    for (std::int32_t local : fn.locals) {
        TypeId type = program->vars[static_cast<std::size_t>(local)].type;
        decls.push_back(Declare(type, var_names[static_cast<std::size_t>(local)]) + ";");
    }
    const tisple::Expr& body = At(fn.body);
    std::string text;
    std::uint32_t count = body.kind == ExprKind::Do ? body.count : 1;
    for (std::uint32_t i = 0; i < count; i++) {
        ExprId item = body.kind == ExprKind::Do ? program->lists[body.first + i] : fn.body;
        bool last = i + 1 == count;
        TypeKind kind = Kind(At(item).type);
        if (!last || fn.ret == kVoidType || kind == TypeKind::Never || kind == TypeKind::Void) {
            text += Stmt(item);
            continue;
        }
        CValue v = Expr(item);
        text += v.pre + Pad() + "return " + v.text + ";\n";
    }
    if (index == program->main && fn.ret == kVoidType) {
        text += "    return 0;\n";
    }
    std::string out = Prototype(index, Params(index)) + " {\n";
    for (const std::string& d : decls) {
        out += "    " + d + "\n";
    }
    return out + text + "}\n";
}

} // namespace tisple::racec
