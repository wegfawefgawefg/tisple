#include "check/arith.hpp"
#include "race/race_c_internal.hpp"

// operators as C with Tisple's results (race_c.hpp): wrapping by casts, unsigned where C's
// int would overflow, shifts and divisions where C leaves the result undefined written out

namespace tisple::racec {

namespace {

const char* OpText(Op op) {
    switch (op) {
    case Op::Add:
        return "+";
    case Op::Sub:
        return "-";
    case Op::Mul:
        return "*";
    case Op::Div:
        return "/";
    case Op::Mod:
        return "%";
    case Op::And:
        return "&";
    case Op::Or:
        return "|";
    case Op::Xor:
        return "^";
    case Op::Shl:
        return "<<";
    case Op::Shr:
        return ">>";
    case Op::Eq:
        return "==";
    case Op::Ne:
        return "!=";
    case Op::Lt:
        return "<";
    case Op::Le:
        return "<=";
    case Op::Gt:
        return ">";
    case Op::Ge:
        return ">=";
    default:
        return "?";
    }
}

bool IsCompare(Op op) {
    return op == Op::Eq || op == Op::Ne || op == Op::Lt || op == Op::Le || op == Op::Gt ||
           op == Op::Ge;
}

// the most a u8 shift count can be: a literal, or (& x k)
std::uint32_t CountBound(const Program& program, ExprId id) {
    const Expr& e = program.exprs[static_cast<std::size_t>(id)];
    if (e.kind == ExprKind::Literal) {
        return e.value & 0xFF;
    }
    if (e.kind == ExprKind::Binary && e.op == Op::And) {
        for (ExprId side : {e.a, e.b}) {
            const Expr& s = program.exprs[static_cast<std::size_t>(side)];
            if (s.kind == ExprKind::Literal) {
                return s.value & 0xFF;
            }
        }
    }
    return 255;
}

// a divisor that cannot be zero: a nonzero literal, or (| x k) with k nonzero
bool NonZero(const Program& program, ExprId id) {
    const Expr& e = program.exprs[static_cast<std::size_t>(id)];
    if (e.kind == ExprKind::Literal) {
        return (e.value & 0xFFFF) != 0;
    }
    if (e.kind == ExprKind::Binary && e.op == Op::Or) {
        for (ExprId side : {e.a, e.b}) {
            const Expr& s = program.exprs[static_cast<std::size_t>(side)];
            if (s.kind == ExprKind::Literal && (s.value & 0xFFFF) != 0) {
                return true;
            }
        }
    }
    return false;
}

std::string Cast(const std::string& type, const std::string& text) {
    return "((" + type + ")(" + text + "))";
}

} // namespace

CValue Writer::Binary(const tisple::Expr& e) {
    TypeId type = At(e.a).type;
    TypeKind kind = Kind(type);
    CValue a = Expr(e.a);
    CValue b = Expr(e.b);
    Order(a, type, b);
    if (e.op == Op::Shl || e.op == Op::Shr) {
        return Shift(e, a, b);
    }
    if (e.op == Op::Div || e.op == Op::Mod) {
        return Divide(e, a, b);
    }
    CValue out;
    out.pre = a.pre + b.pre;
    std::string op = OpText(e.op);
    std::string t = CType(type);
    if (IsCompare(e.op)) {
        out.text = "(" + a.text + " " + op + " " + b.text + ")";
        return out;
    }
    if (e.op == Op::Mul && kind == TypeKind::I16) {
        out.text = Cast(t, "(unsigned)" + a.text + " * (unsigned)" + b.text);
    } else if (e.op == Op::Mul && kind != TypeKind::I8) {
        // u8 * u8 is past a 16-bit int's range; unsigned wraps
        out.text = Cast(t, "(unsigned)" + a.text + " * " + b.text);
    } else if (kind == TypeKind::I16 && (e.op == Op::Add || e.op == Op::Sub)) {
        out.text = Cast(t, "(uint16_t)" + a.text + " " + op + " (uint16_t)" + b.text);
    } else {
        out.text = Cast(t, a.text + " " + op + " " + b.text);
    }
    return out;
}

CValue Writer::Shift(const tisple::Expr& e, CValue a, CValue b) {
    TypeKind kind = Kind(At(e.a).type);
    std::string t = CType(At(e.a).type);
    std::uint32_t bits = KindBits(kind);
    bool is_signed = IsSignedKind(kind);
    bool literal = At(e.b).kind == ExprKind::Literal;
    std::uint32_t bound = CountBound(*program, e.b);
    CValue out;
    if (literal && bound >= bits && (e.op == Op::Shl || !is_signed)) {
        out.pre = a.pre + b.pre;
        out.text = "((" + t + ")0)";
        return out;
    }
    // counts below 16 are defined on a 16-bit int; past that Tisple's results are spelled out
    bool guard = !literal && bound >= 16;
    if (guard) {
        b = Stabilize(b, kU8Type);
    }
    out.pre = a.pre + b.pre;
    std::string count = b.text;
    if (e.op == Op::Shr && is_signed) {
        if (literal && bound >= bits) {
            count = std::to_string(bits - 1);
        } else if (guard) {
            count = "(" + b.text + " > 15 ? 15 : " + b.text + ")";
        }
        out.text = Cast(t, a.text + " >> " + count);
        return out;
    }
    std::string core;
    if (e.op == Op::Shr) {
        core = Cast(t, a.text + " >> " + count);
    } else if (kind == TypeKind::I8) {
        core = Cast(t, "(unsigned)(uint8_t)" + a.text + " << " + count);
    } else if (kind == TypeKind::U8 && literal) {
        core = Cast(t, a.text + " << " + count);
    } else {
        core = Cast(t, "(unsigned)" + a.text + " << " + count);
    }
    out.text = guard ? "(" + b.text + " > 15 ? (" + t + ")0 : " + core + ")" : core;
    return out;
}

CValue Writer::Divide(const tisple::Expr& e, CValue a, CValue b) {
    TypeId type = At(e.a).type;
    TypeKind kind = Kind(type);
    std::string t = CType(type);
    bool is_div = e.op == Op::Div;
    std::string op = is_div ? " / " : " % ";
    bool literal = At(e.b).kind == ExprKind::Literal;
    std::int32_t divisor =
        literal ? SignedValue(kind, static_cast<std::uint16_t>(At(e.b).value)) : 1;
    bool zero_possible = !NonZero(*program, e.b);
    // -32768 / -1 overflows a 16-bit int; Tisple wraps it to -32768, and % gives 0
    bool minus_one_possible = kind == TypeKind::I16 && (!literal || divisor == -1);
    CValue out;
    if (literal && divisor == 0) {
        // check/arith.hpp: x % 0 is x; x / 0 is all ones, or 1 for a negative signed x
        if (IsSignedKind(kind) && is_div) {
            a = Stabilize(a, type);
        }
        out.pre = a.pre + b.pre;
        if (!is_div) {
            out.text = a.text;
        } else if (IsSignedKind(kind)) {
            out.text = Cast(t, a.text + " < 0 ? 1 : -1");
        } else {
            out.text = Cast(t, Literal(type, 0xFFFF));
        }
        return out;
    }
    if (!zero_possible && !minus_one_possible) {
        out.pre = a.pre + b.pre;
        out.text = Cast(t, a.text + op + b.text);
        return out;
    }
    a = Stabilize(a, type);
    b = Stabilize(b, type);
    out.pre = a.pre + b.pre;
    std::string text = Cast(t, a.text + op + b.text);
    if (minus_one_possible) {
        std::string negated = is_div ? Cast(t, "0u - (unsigned)" + a.text) : "0";
        text = b.text + " == -1 ? " + negated + " : " + text;
    }
    if (zero_possible) {
        std::string by_zero = a.text;
        if (is_div) {
            by_zero = IsSignedKind(kind) ? "(" + a.text + " < 0 ? 1 : -1)" : Literal(type, 0xFFFF);
        }
        text = b.text + " == 0 ? " + by_zero + " : " + text;
    }
    out.text = Cast(t, text);
    return out;
}

} // namespace tisple::racec
