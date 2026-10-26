#include "check/checker.hpp"

namespace tisple {

namespace {

struct OpInfo {
    const char* name;
    Op op;
};

const OpInfo kBinaryOps[] = {
    {"+", Op::Add},  {"-", Op::Sub},  {"*", Op::Mul}, {"/", Op::Div},
    {"%", Op::Mod},  {"&", Op::And},  {"|", Op::Or},  {"^", Op::Xor},
    {"<<", Op::Shl}, {">>", Op::Shr}, {"=", Op::Eq},  {"!=", Op::Ne},
    {"<", Op::Lt},   {"<=", Op::Le},  {">", Op::Gt},  {">=", Op::Ge},
};

bool IsComparison(Op op) {
    return op == Op::Eq || op == Op::Ne || op == Op::Lt || op == Op::Le || op == Op::Gt ||
           op == Op::Ge;
}

// the narrowest integer type that holds value, for fixes
const char* WiderFor(std::int64_t value) {
    if (value < 0) {
        return value >= -32768 ? "i16" : "u16";
    }
    return "u16";
}

bool IsTyped(TypeId type) {
    return type > kErrorType;
}

// arithmetic takes integers, and a pointer on the left of + and -
bool OperandKindOk(const Checker& ck, Op op, TypeId type, bool left) {
    if (IsComparison(op) || !IsTyped(type)) {
        return true;
    }
    TypeKind kind = KindOf(*ck.out, type);
    bool pointer_math = (op == Op::Add || op == Op::Sub) && left && kind == TypeKind::Ptr;
    return IsIntegerKind(kind) || pointer_math;
}

// operands of a binary form: a bare literal takes the other side's type
void CheckOperands(Checker& ck, const Node& node, Op op, TypeId hint, ExprId& a, ExprId& b) {
    const Node& left = node.items[1];
    const Node& right = node.items[2];
    TypeId operand_hint = IsComparison(op) ? -1 : hint;
    bool pointer_math = op == Op::Add || op == Op::Sub;
    if (!IsBareLiteral(left)) {
        a = CheckExpr(ck, left, operand_hint);
        TypeId ta = ExprType(ck, a);
        TypeId right_hint = ta;
        if (pointer_math && IsTyped(ta) && KindOf(*ck.out, ta) == TypeKind::Ptr) {
            right_hint = kU16Type;
        }
        if (IsBareLiteral(right) && !OperandKindOk(ck, op, ta, true)) {
            b = ErrorExpr(ck, right);
            return;
        }
        if (IsBareLiteral(right) && IsTyped(right_hint) &&
            IsIntegerKind(KindOf(*ck.out, right_hint)) &&
            !LiteralFits(KindOf(*ck.out, right_hint), right.value)) {
            std::string type = ShowType(ck, right_hint);
            b = Fail(ck, right,
                     right.text + " does not fit in " + type + "; write (" + node.items[0].text +
                         " (as " + WiderFor(right.value) + " " + Show(left) + ") " + right.text +
                         ")");
            return;
        }
        b = CheckExpr(ck, right, right_hint);
        return;
    }
    if (!IsBareLiteral(right)) {
        b = CheckExpr(ck, right, operand_hint);
        TypeId tb = ExprType(ck, b);
        if (IsTyped(tb) && KindOf(*ck.out, tb) == TypeKind::Ptr) {
            a = Fail(ck, node,
                     "the pointer goes first; write (" + node.items[0].text + " " + Show(right) +
                         " " + left.text + ")");
            return;
        }
        if (!OperandKindOk(ck, op, tb, false)) {
            a = ErrorExpr(ck, left);
            return;
        }
        if (IsTyped(tb) && IsIntegerKind(KindOf(*ck.out, tb)) &&
            !LiteralFits(KindOf(*ck.out, tb), left.value)) {
            a = Fail(ck, left,
                     left.text + " does not fit in " + ShowType(ck, tb) + "; write (" +
                         node.items[0].text + " " + left.text + " (as " + WiderFor(left.value) +
                         " " + Show(right) + "))");
            return;
        }
        a = CheckExpr(ck, left, tb);
        return;
    }
    if (operand_hint < 0) {
        a = Fail(ck, node,
                 Show(node) + ": neither side has a type; a literal takes the type of what it "
                              "meets, so use a typed value on one side");
        b = ErrorExpr(ck, right);
        return;
    }
    a = CheckExpr(ck, left, operand_hint);
    b = CheckExpr(ck, right, operand_hint);
}

std::string KindWord(const Checker& ck, TypeId type) {
    TypeKind kind = KindOf(*ck.out, type);
    if (kind == TypeKind::Bool) {
        return "bool";
    }
    if (kind == TypeKind::Enum) {
        return "the enum " + ShowType(ck, type);
    }
    if (kind == TypeKind::Ptr) {
        return "a pointer";
    }
    if (kind == TypeKind::Void) {
        return "nothing (that form gives no value)";
    }
    return ShowType(ck, type);
}

// (+ (as u16 a) b): widen the narrower side, or convert the right side to the left's type
std::string MismatchFix(const Checker& ck, const Node& node, TypeId ta, TypeId tb) {
    TypeKind ka = KindOf(*ck.out, ta);
    TypeKind kb = KindOf(*ck.out, tb);
    const std::string& head = node.items[0].text;
    std::string left = Show(node.items[1]);
    std::string right = Show(node.items[2]);
    if (!IsIntegerKind(ka) || !IsIntegerKind(kb)) {
        return "";
    }
    if (KindBits(ka) < KindBits(kb)) {
        return "; write (" + head + " (as " + ShowType(ck, tb) + " " + left + ") " + right + ")";
    }
    return "; write (" + head + " " + left + " (as " + ShowType(ck, ta) + " " + right + "))";
}

// (+ p x) with x : u8 is (+ p (as u16 x)): the same Convert node, so the backend sees one tree
ExprId WidenCount(Checker& ck, const Node& count_node, ExprId count) {
    ExprId id = AddExpr(ck, ExprKind::Convert, kU16Type, count_node);
    ExprAt(ck, id).a = count;
    return id;
}

// (<< x n): x any integer, n always u8
ExprId CheckShift(Checker& ck, const Node& node, Op op, TypeId hint) {
    const std::string& head = node.items[0].text;
    const Node& left = node.items[1];
    const Node& right = node.items[2];
    ExprId a = -1;
    if (IsBareLiteral(left) && hint < 0) {
        a = Fail(ck, left,
                 Show(node) +
                     ": the shifted literal has no type; give the result one, as in "
                     "(let m u8 " +
                     Show(node) + ")");
    } else {
        a = CheckExpr(ck, left, hint);
    }
    ExprId b = CheckExpr(ck, right, kU8Type);
    TypeId ta = ExprType(ck, a);
    if (IsTyped(ta) && !IsIntegerKind(KindOf(*ck.out, ta))) {
        return Fail(ck, node, head + " works on integers, not " + KindWord(ck, ta));
    }
    bool count_ok = RequireType(ck, right, b, kU8Type, "the shift count");
    if (!IsTyped(ta) || !IsTyped(ExprType(ck, b)) || !count_ok) {
        return ErrorExpr(ck, node);
    }
    ExprId id = AddExpr(ck, ExprKind::Binary, ta, node);
    Expr& expr = ExprAt(ck, id);
    expr.op = op;
    expr.a = a;
    expr.b = b;
    return id;
}

} // namespace

bool FindBinaryOp(const std::string& name, Op& op) {
    for (const OpInfo& info : kBinaryOps) {
        if (name == info.name) {
            op = info.op;
            return true;
        }
    }
    return false;
}

void LiteralRange(TypeKind kind, std::int64_t& low, std::int64_t& high) {
    switch (kind) {
    case TypeKind::U8:
        low = 0;
        high = 255;
        return;
    case TypeKind::I8:
        low = -128;
        high = 127;
        return;
    case TypeKind::U16:
        low = 0;
        high = 65535;
        return;
    default:
        low = -32768;
        high = 32767;
        return;
    }
}

bool LiteralFits(TypeKind kind, std::int64_t value) {
    std::int64_t low = 0;
    std::int64_t high = 0;
    LiteralRange(kind, low, high);
    return value >= low && value <= high;
}

ExprId CheckBinary(Checker& ck, const Node& node, Op op, TypeId hint) {
    const std::string& head = node.items[0].text;
    if (node.items.size() != 3) {
        std::string fix = node.items.size() > 3 ? "; write (" + head + " (" + head + " a b) c)"
                                                : "; write (" + head + " a b)";
        return Fail(ck, node, head + " takes two operands" + fix);
    }
    if (op == Op::Shl || op == Op::Shr) {
        return CheckShift(ck, node, op, hint);
    }
    ExprId a = -1;
    ExprId b = -1;
    CheckOperands(ck, node, op, hint, a, b);
    TypeId ta = ExprType(ck, a);
    TypeId tb = ExprType(ck, b);
    if (!OperandKindOk(ck, op, ta, true) || !OperandKindOk(ck, op, tb, false)) {
        TypeId bad = OperandKindOk(ck, op, ta, true) ? tb : ta;
        return Fail(ck, node, head + " works on integers, not " + KindWord(ck, bad));
    }
    if (!IsTyped(ta) || !IsTyped(tb)) {
        return ErrorExpr(ck, node);
    }
    TypeKind ka = KindOf(*ck.out, ta);
    if ((op == Op::Add || op == Op::Sub) && ka == TypeKind::Ptr) {
        if (tb == kU8Type) {
            b = WidenCount(ck, node.items[2], b);
        } else if (tb != kU16Type) {
            // OperandKindOk let only integers through: i8 or i16 here
            return Fail(ck, node.items[2],
                        "a pointer moves by a u8 or u16 count of elements; " + Show(node.items[2]) +
                            " is " + ShowType(ck, tb) + "; write (" + head + " " +
                            Show(node.items[1]) + " (as u16 " + Show(node.items[2]) + "))");
        }
        ExprId id = AddExpr(ck, op == Op::Add ? ExprKind::PtrAdd : ExprKind::PtrSub, ta, node);
        Expr& expr = ExprAt(ck, id);
        expr.a = a;
        expr.b = b;
        expr.value = TypeOf(*ck.out, TypeOf(*ck.out, ta).elem).size;
        return id;
    }
    if (ta != tb) {
        return Fail(ck, node,
                    ShowType(ck, ta) + " " + head + " " + ShowType(ck, tb) + ": types must match" +
                        MismatchFix(ck, node, ta, tb));
    }
    bool equality = op == Op::Eq || op == Op::Ne;
    bool ordered = IsComparison(op) && !equality;
    bool ok =
        IsIntegerKind(ka) || (equality && IsScalarKind(ka)) || (ordered && ka == TypeKind::Ptr);
    if (!ok && equality) {
        return Fail(ck, node,
                    head + " compares integers, bools, enum members and pointers, not " +
                        KindWord(ck, ta));
    }
    if (!ok) {
        std::string fix = ka == TypeKind::Ptr ? "; convert with (as u16 p) first" : "";
        return Fail(ck, node, head + " works on integers, not " + KindWord(ck, ta) + fix);
    }
    ExprId id = AddExpr(ck, ExprKind::Binary, IsComparison(op) ? kBoolType : ta, node);
    Expr& expr = ExprAt(ck, id);
    expr.op = op;
    expr.a = a;
    expr.b = b;
    return id;
}

ExprId CheckUnary(Checker& ck, const Node& node, TypeId hint) {
    const std::string& head = node.items[0].text;
    if (node.items.size() != 2) {
        return Fail(ck, node, head + " takes one operand; write (" + head + " x)");
    }
    bool is_not = head == "not";
    ExprId a = CheckExpr(ck, node.items[1], is_not ? kBoolType : hint);
    TypeId type = ExprType(ck, a);
    if (!IsTyped(type)) {
        return ErrorExpr(ck, node);
    }
    if (is_not && type != kBoolType) {
        return Fail(ck, node,
                    "not works on bool, not " + KindWord(ck, type) +
                        "; write (= " + Show(node.items[1]) + " 0)");
    }
    if (!is_not && !IsIntegerKind(KindOf(*ck.out, type))) {
        return Fail(ck, node, "~ works on integers, not " + KindWord(ck, type));
    }
    ExprId id = AddExpr(ck, ExprKind::Unary, type, node);
    ExprAt(ck, id).op = is_not ? Op::Not : Op::BitNot;
    ExprAt(ck, id).a = a;
    return id;
}

ExprId CheckLogic(Checker& ck, const Node& node) {
    const std::string& head = node.items[0].text;
    if (node.items.size() != 3) {
        return Fail(ck, node,
                    head + " takes two operands; write (" + head + " (" + head + " a b) c)");
    }
    ExprId a = CheckExpr(ck, node.items[1], kBoolType);
    ExprId b = CheckExpr(ck, node.items[2], kBoolType);
    bool ok = RequireType(ck, node.items[1], a, kBoolType, head);
    ok = RequireType(ck, node.items[2], b, kBoolType, head) && ok;
    if (!ok) {
        return ErrorExpr(ck, node);
    }
    ExprId id = AddExpr(ck, head == "and" ? ExprKind::And : ExprKind::Or, kBoolType, node);
    ExprAt(ck, id).a = a;
    ExprAt(ck, id).b = b;
    return id;
}

} // namespace tisple
