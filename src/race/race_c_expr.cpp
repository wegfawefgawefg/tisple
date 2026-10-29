#include "check/arith.hpp"
#include "race/race_c_internal.hpp"

#include <cstdio>

// expressions and places as C (race_c.hpp)

namespace tisple::racec {

const Expr& Writer::At(ExprId id) const {
    return program->exprs[static_cast<std::size_t>(id)];
}

TypeKind Writer::Kind(TypeId type) const {
    return KindOf(*program, type);
}

bool Writer::Fail(const std::string& why) {
    if (error.empty()) {
        error = why;
    }
    return false;
}

std::string Writer::Pad() const {
    return std::string(static_cast<std::size_t>(indent) * 4, ' ');
}

std::string Writer::CType(TypeId type) const {
    const Type& t = TypeOf(*program, type);
    switch (t.kind) {
    case TypeKind::Bool:
    case TypeKind::U8:
    case TypeKind::Enum:
        return "uint8_t";
    case TypeKind::I8:
        return "int8_t";
    case TypeKind::U16:
        return "uint16_t";
    case TypeKind::I16:
        return "int16_t";
    case TypeKind::Ptr:
        return CType(t.elem) + " *";
    case TypeKind::Struct:
        return "struct " + program->structs[static_cast<std::size_t>(t.decl)].name;
    default:
        return "void";
    }
}

std::string Writer::Declare(TypeId type, const std::string& name) const {
    std::string type_text = CType(type);
    return type_text + (type_text.back() == '*' ? "" : " ") + name;
}

std::string Writer::NewTemp(TypeId type) {
    std::string name = "tmp" + std::to_string(temps++);
    decls.push_back(Declare(type, name) + ";");
    return name;
}

CValue Writer::Stabilize(CValue v, TypeId type) {
    if (v.stable) {
        return v;
    }
    std::string name = NewTemp(type);
    v.pre += Pad() + name + " = " + v.text + ";\n";
    v.text = name;
    v.stable = true;
    return v;
}

void Writer::Order(CValue& a, TypeId a_type, CValue& b) {
    if (!b.pre.empty()) {
        a = Stabilize(a, a_type);
    }
}

std::string Writer::Literal(TypeId type, std::uint32_t raw) const {
    TypeKind kind = Kind(type);
    std::uint16_t bits = static_cast<std::uint16_t>(raw & 0xFFFF);
    if (kind == TypeKind::Ptr) {
        char text[48];
        std::snprintf(text, sizeof(text), "((%s)0x%04X)", CType(type).c_str(), bits);
        return text;
    }
    if (kind == TypeKind::I8 || kind == TypeKind::I16) {
        std::int32_t value = SignedValue(kind, bits);
        if (value == -32768) {
            return "(-32767 - 1)";
        }
        return value < 0 ? "(" + std::to_string(value) + ")" : std::to_string(value);
    }
    if (KindBits(kind) == 8) {
        return std::to_string(bits & 0xFF);
    }
    // 32768 and up are long in a 16-bit int's C; u makes them unsigned int
    return std::to_string(bits) + (bits > 32767 ? "u" : "");
}

bool IsIdentifier(const std::string& text) {
    if (text.empty() || (text[0] >= '0' && text[0] <= '9')) {
        return false;
    }
    for (char c : text) {
        bool ok =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string PointerAt(const std::string& pointer, const std::string& count) {
    // Oscar64 indexes only names: (p + n)[86] is "Array expected for indexing" there
    if (IsIdentifier(pointer)) {
        return pointer + "[" + count + "]";
    }
    return "(*(" + pointer + " + " + count + "))";
}

std::string FieldName(const Program& program, TypeId struct_type, std::uint32_t offset) {
    const Type& t = TypeOf(program, struct_type);
    if (t.kind != TypeKind::Struct) {
        return "?";
    }
    for (const Field& field : program.structs[static_cast<std::size_t>(t.decl)].fields) {
        if (field.offset == offset) {
            return field.name;
        }
    }
    return "?";
}

CValue Writer::Place(ExprId id, bool stabilize) {
    const tisple::Expr& e = At(id);
    CValue out;
    switch (e.kind) {
    case ExprKind::VarRef: {
        const Var& var = program->vars[static_cast<std::size_t>(e.ref)];
        out.text = var_names[static_cast<std::size_t>(e.ref)];
        out.stable = var.kind == VarKind::Param || var.kind == VarKind::Local;
        return out;
    }
    case ExprKind::Field: {
        std::string name =
            program->structs[static_cast<std::size_t>(TypeOf(*program, At(e.a).type).decl)]
                .fields[static_cast<std::size_t>(e.ref)]
                .name;
        if (At(e.a).kind == ExprKind::Load) {
            CValue p = Expr(At(e.a).a);
            if (stabilize) {
                p = Stabilize(p, At(At(e.a).a).type);
            }
            out.pre = p.pre;
            out.text = p.text + "->" + name;
            return out;
        }
        CValue base = Place(e.a, stabilize);
        out.pre = base.pre;
        out.text = base.text + "." + name;
        return out;
    }
    case ExprKind::Index: {
        if (Kind(At(e.a).type) != TypeKind::Array) {
            Fail("an index into something other than an array");
            return out;
        }
        // the index first, so the array's own indices know whether to be kept
        CValue index = Expr(e.b);
        CValue base = Place(e.a, stabilize || !index.pre.empty());
        if (stabilize) {
            index = Stabilize(index, At(e.b).type);
        }
        out.pre = base.pre + index.pre;
        out.text = base.text + "[" + index.text + "]";
        return out;
    }
    case ExprKind::Load: {
        const tisple::Expr& p = At(e.a);
        if (p.kind == ExprKind::PtrAdd) {
            CValue base = Expr(p.a);
            CValue count = Expr(p.b);
            Order(base, At(p.a).type, count);
            if (stabilize) {
                base = Stabilize(base, At(p.a).type);
                count = Stabilize(count, At(p.b).type);
            }
            out.pre = base.pre + count.pre;
            out.text = PointerAt(base.text, count.text);
            return out;
        }
        CValue pointer = Expr(e.a);
        if (stabilize) {
            pointer = Stabilize(pointer, p.type);
        }
        out.pre = pointer.pre;
        out.text = "(*" + pointer.text + ")";
        return out;
    }
    case ExprKind::LoadField: {
        CValue pointer = Expr(e.a);
        if (stabilize) {
            pointer = Stabilize(pointer, At(e.a).type);
        }
        TypeId elem = TypeOf(*program, At(e.a).type).elem;
        out.pre = pointer.pre;
        out.text = pointer.text + "->" + FieldName(*program, elem, e.value);
        return out;
    }
    default:
        Fail("not a place");
        return out;
    }
}

CValue Writer::Call(const tisple::Expr& e, bool as_value) {
    std::vector<CValue> args;
    for (std::uint32_t i = 0; i < e.count; i++) {
        args.push_back(Expr(program->lists[e.first + i]));
    }
    // an argument a later argument's statements could change is kept first
    bool later = false;
    for (std::size_t i = args.size(); i-- > 0;) {
        if (later) {
            args[i] = Stabilize(args[i], At(program->lists[e.first + i]).type);
        }
        later = later || !args[i].pre.empty();
    }
    CValue out;
    std::string call = fn_names[static_cast<std::size_t>(e.ref)] + "(";
    for (std::size_t i = 0; i < args.size(); i++) {
        out.pre += args[i].pre;
        call += (i == 0 ? "" : ", ") + args[i].text;
    }
    call += ")";
    if (!as_value) {
        out.text = call;
        return out;
    }
    std::string temp = NewTemp(e.type);
    out.pre += Pad() + temp + " = " + call + ";\n";
    out.text = temp;
    out.stable = true;
    return out;
}

CValue Writer::Logic(const tisple::Expr& e) {
    bool is_and = e.kind == ExprKind::And;
    CValue a = Expr(e.a);
    indent++;
    CValue b = Expr(e.b);
    indent--;
    CValue out;
    if (b.pre.empty()) {
        out.pre = a.pre;
        out.text = "(" + a.text + (is_and ? " && " : " || ") + b.text + ")";
        return out;
    }
    std::string temp = NewTemp(kBoolType);
    out.pre = a.pre + Pad() + temp + " = " + a.text + ";\n";
    out.pre += Pad() + "if (" + (is_and ? "" : "!") + temp + ") {\n" + b.pre;
    out.pre += Pad() + "    " + temp + " = " + b.text + ";\n" + Pad() + "}\n";
    out.text = temp;
    out.stable = true;
    return out;
}

CValue Writer::IfValue(const tisple::Expr& e) {
    CValue cond = Expr(e.a);
    indent++;
    CValue x = Expr(e.b);
    CValue y = Expr(e.c);
    indent--;
    CValue out;
    std::string type = CType(e.type);
    if (x.pre.empty() && y.pre.empty()) {
        out.pre = cond.pre;
        out.text = "((" + type + ")(" + cond.text + " ? " + x.text + " : " + y.text + "))";
        return out;
    }
    std::string temp = NewTemp(e.type);
    out.pre = cond.pre + Pad() + "if (" + cond.text + ") {\n" + x.pre;
    out.pre += Pad() + "    " + temp + " = " + x.text + ";\n" + Pad() + "} else {\n" + y.pre;
    out.pre += Pad() + "    " + temp + " = " + y.text + ";\n" + Pad() + "}\n";
    out.text = temp;
    out.stable = true;
    return out;
}

CValue Writer::MatchValue(const tisple::Expr& e) {
    CValue out;
    std::string temp = NewTemp(e.type);
    out.pre = MatchStmt(e, temp);
    out.text = temp;
    out.stable = true;
    return out;
}

CValue Writer::DoValue(const tisple::Expr& e) {
    CValue out;
    if (e.count == 0) {
        out.text = "0";
        out.stable = true;
        return out;
    }
    for (std::uint32_t i = 0; i + 1 < e.count; i++) {
        out.pre += Stmt(program->lists[e.first + i]);
    }
    CValue last = Expr(program->lists[e.first + e.count - 1]);
    out.pre += last.pre;
    out.text = last.text;
    out.stable = last.stable;
    return out;
}

CValue Writer::Expr(ExprId id) {
    const tisple::Expr& e = At(id);
    CValue out;
    switch (e.kind) {
    case ExprKind::Literal:
        out.text = Literal(e.type, e.value);
        out.stable = true;
        return out;
    case ExprKind::VarRef:
    case ExprKind::Index:
    case ExprKind::Field:
    case ExprKind::Load:
    case ExprKind::LoadField:
        return Place(id, false);
    case ExprKind::AddrOf: {
        CValue place = Place(e.a, false);
        out.pre = place.pre;
        out.text = "(&" + place.text + ")";
        return out;
    }
    case ExprKind::Unary: {
        CValue a = Expr(e.a);
        out.pre = a.pre;
        if (e.op == Op::Not) {
            out.text = "(!" + a.text + ")";
        } else {
            out.text = "((" + CType(e.type) + ")~" + a.text + ")";
        }
        return out;
    }
    case ExprKind::Binary:
        return Binary(e);
    case ExprKind::And:
    case ExprKind::Or:
        return Logic(e);
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub: {
        CValue a = Expr(e.a);
        CValue b = Expr(e.b);
        Order(a, At(e.a).type, b);
        out.pre = a.pre + b.pre;
        out.text = "(" + a.text + (e.kind == ExprKind::PtrAdd ? " + " : " - ") + b.text + ")";
        return out;
    }
    case ExprKind::Convert: {
        CValue a = Expr(e.a);
        out.pre = a.pre;
        out.text = "((" + CType(e.type) + ")" + a.text + ")";
        return out;
    }
    case ExprKind::Call:
        return Call(e, true);
    case ExprKind::If:
        if (e.c < 0) {
            Fail("an if without else used as a value");
            return out;
        }
        return IfValue(e);
    case ExprKind::Do:
        return DoValue(e);
    case ExprKind::Match:
        return MatchValue(e);
    case ExprKind::Reinterpret:
        Fail("as between u16 and a pointer has no faithful C form here");
        return out;
    case ExprKind::IoRead:
    case ExprKind::IoWrite:
        Fail("io_read and io_write have no C form in the race");
        return out;
    case ExprKind::Jsr:
    case ExprKind::JsrRegs:
        Fail("jsr has no C form in the race");
        return out;
    case ExprKind::WaitCycles:
        Fail("wait_cycles has no C form in the race");
        return out;
    case ExprKind::StructLit:
        Fail("a struct value outside a let, set or store");
        return out;
    default:
        // a statement where a value was asked for: its value is never read
        out.pre = Stmt(id);
        out.text = "0";
        out.stable = true;
        return out;
    }
}

} // namespace tisple::racec
