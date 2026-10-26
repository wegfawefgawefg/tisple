#include "check/arith.hpp"
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

bool FindBinaryOp(const std::string& name, Op& op) {
    for (const OpInfo& info : kBinaryOps) {
        if (name == info.name) {
            op = info.op;
            return true;
        }
    }
    return false;
}

bool IsComparison(Op op) {
    return op == Op::Eq || op == Op::Ne || op == Op::Lt || op == Op::Le || op == Op::Gt ||
           op == Op::Ge;
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

ExprId MakeLiteral(Checker& ck, const Node& node, TypeId type, std::uint16_t raw) {
    ExprId id = AddExpr(ck, ExprKind::Literal, type, node);
    ExprAt(ck, id).value = raw;
    return id;
}

std::string UnknownName(Checker& ck, const std::string& text, const char* what) {
    // a name some import has is the likely fix
    const ModuleScope& scope = ck.modules[static_cast<std::size_t>(ck.at.module)];
    for (const auto& [prefix, module] : scope.imports) {
        const ModuleScope& target = ck.modules[static_cast<std::size_t>(module)];
        if (target.names.count(text) != 0) {
            return std::string("unknown ") + what + " " + text + "; did you mean " + prefix + "." +
                   text + "?";
        }
    }
    return std::string("unknown ") + what + " " + text;
}

ExprId CheckName(Checker& ck, const Node& node) {
    if (node.text == "true" || node.text == "false") {
        return MakeLiteral(ck, node, kBoolType, node.text == "true" ? 1 : 0);
    }
    std::vector<std::string> segments = SplitDotted(node.text);
    if (segments.empty()) {
        return Fail(ck, node, node.text + " is not a name; dotted names look like dot.x");
    }
    NameRef ref = LookupName(ck, segments);
    const std::string& first = segments[0];
    ExprId value = -1;
    switch (ref.kind) {
    case NameKind::None:
        if (!ref.missing_in.empty()) {
            return Fail(ck, node, ref.missing_in + " has no " + segments[ref.used]);
        }
        if (ref.used > 0) {
            return Fail(ck, node, first + " is a macro; call it as (" + first + " ...)");
        }
        return Fail(ck, node, UnknownName(ck, node.text, "name"));
    case NameKind::Module:
        return Fail(ck, node,
                    first + " is a module; name one of its members, as in " + first + ".x");
    case NameKind::Function:
        return Fail(ck, node, node.text + " is a function; call it: (" + node.text + " ...)");
    case NameKind::Struct:
        return Fail(ck, node,
                    node.text + " is a type; build a value with (" + node.text +
                        " :field value ...)");
    case NameKind::Enum:
        return Fail(ck, node, node.text + " is an enum; name a member, as in " + node.text + ".x");
    case NameKind::EnumMember:
        value = MakeLiteral(ck, node, EnumType(ck, ref.index), ref.value);
        break;
    case NameKind::Const: {
        EvalConst(ck, ref.index);
        const ConstDecl& cd = ck.consts[static_cast<std::size_t>(ref.index)];
        if (cd.state != Progress::Done) {
            return ErrorExpr(ck, node);
        }
        if (cd.var >= 0) {
            value = AddExpr(ck, ExprKind::VarRef, cd.type, node);
            ExprAt(ck, value).ref = cd.var;
        } else {
            value = MakeLiteral(ck, node, cd.type, cd.value);
        }
        break;
    }
    case NameKind::Var: {
        const Var& var = ck.out->vars[static_cast<std::size_t>(ref.index)];
        value = AddExpr(ck, ExprKind::VarRef, var.type, node);
        ExprAt(ck, value).ref = ref.index;
        break;
    }
    }
    if (ref.used < segments.size() && ref.kind != NameKind::Var) {
        std::string base = segments[0];
        for (std::size_t i = 1; i < ref.used; i++) {
            base += "." + segments[i];
        }
        return Fail(ck, node, base + " is a constant; it has no field " + segments[ref.used]);
    }
    for (std::size_t i = ref.used; i < segments.size(); i++) {
        value = FieldAccess(ck, node, value, segments[i]);
    }
    return value;
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
        if (IsBareLiteral(right) && IsTyped(right_hint) &&
            IsIntegerKind(KindOf(*ck.out, right_hint)) &&
            !LiteralFits(KindOf(*ck.out, right_hint), right.value)) {
            std::string type = ShowType(ck, right_hint);
            b = Fail(ck, right,
                     right.text + " does not fit in " + type + "; write (" + node.items[0].text +
                         " (" + WiderFor(right.value) + " " + Show(left) + ") " + right.text + ")");
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
        if (IsTyped(tb) && IsIntegerKind(KindOf(*ck.out, tb)) &&
            !LiteralFits(KindOf(*ck.out, tb), left.value)) {
            a = Fail(ck, left,
                     left.text + " does not fit in " + ShowType(ck, tb) + "; write (" +
                         node.items[0].text + " " + left.text + " (" + WiderFor(left.value) + " " +
                         Show(right) + "))");
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

// (+ (u16 a) b): widen the narrower side, or convert the right side to the left's type
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
        return "; write (" + head + " (" + ShowType(ck, tb) + " " + left + ") " + right + ")";
    }
    return "; write (" + head + " " + left + " (" + ShowType(ck, ta) + " " + right + "))";
}

ExprId CheckBinary(Checker& ck, const Node& node, Op op, TypeId hint) {
    const std::string& head = node.items[0].text;
    if (node.items.size() != 3) {
        std::string fix = node.items.size() > 3 ? "; write (" + head + " (" + head + " a b) c)"
                                                : "; write (" + head + " a b)";
        return Fail(ck, node, head + " takes two operands" + fix);
    }
    ExprId a = -1;
    ExprId b = -1;
    CheckOperands(ck, node, op, hint, a, b);
    TypeId ta = ExprType(ck, a);
    TypeId tb = ExprType(ck, b);
    if (!IsTyped(ta) || !IsTyped(tb)) {
        return ErrorExpr(ck, node);
    }
    TypeKind ka = KindOf(*ck.out, ta);
    if ((op == Op::Add || op == Op::Sub) && ka == TypeKind::Ptr) {
        if (tb != kU16Type) {
            return Fail(ck, node.items[2],
                        "a pointer moves by a u16 count of elements; write (" + head + " " +
                            Show(node.items[1]) + " (u16 " + Show(node.items[2]) + "))");
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
    bool ok = RequireType(ck, node.items[1], a, kBoolType, head + " needs bool operands;");
    ok = RequireType(ck, node.items[2], b, kBoolType, head + " needs bool operands;") && ok;
    if (!ok) {
        return ErrorExpr(ck, node);
    }
    ExprId id = AddExpr(ck, head == "and" ? ExprKind::And : ExprKind::Or, kBoolType, node);
    ExprAt(ck, id).a = a;
    ExprAt(ck, id).b = b;
    return id;
}

ExprId CheckConvert(Checker& ck, const Node& node, TypeId to) {
    const std::string& head = node.items[0].text;
    if (node.items.size() != 2) {
        return Fail(ck, node, head + " converts one value; write (" + head + " x)");
    }
    const Node& arg = node.items[1];
    if (IsBareLiteral(arg)) {
        return Fail(ck, node,
                    Show(node) +
                        " converts nothing: a literal takes its type from where it is "
                        "used; write " +
                        arg.text);
    }
    ExprId a = CheckExpr(ck, arg, -1);
    TypeId from = ExprType(ck, a);
    if (!IsTyped(from)) {
        return ErrorExpr(ck, node);
    }
    TypeKind kind = KindOf(*ck.out, from);
    if (kind == TypeKind::Ptr) {
        return Fail(ck, node,
                    head + " converts integers; for a pointer write (as u16 " + Show(arg) + ")");
    }
    if (kind == TypeKind::Bool) {
        return Fail(ck, node,
                    head + " converts integers, not bool; write (if " + Show(arg) + " 1 0)");
    }
    if (!IsIntegerKind(kind)) {
        return Fail(ck, node, head + " converts integers, not " + KindWord(ck, from));
    }
    ExprId id = AddExpr(ck, ExprKind::Convert, to, node);
    ExprAt(ck, id).a = a;
    return id;
}

ExprId CheckList(Checker& ck, const Node& node, TypeId hint) {
    if (node.items.empty()) {
        return Fail(ck, node, "() is not a form; remove it");
    }
    if (node.mark != QuoteMark::None) {
        return Fail(ck, node,
                    std::string(QuoteMarkText(node.mark)) + " only works inside a macro template");
    }
    const Node& head_node = node.items[0];
    if (head_node.kind != NodeKind::Symbol) {
        return Fail(ck, node, Show(node) + " is not a call; a call starts with a function name");
    }
    const std::string& head = head_node.text;
    Op op = Op::Add;
    if (FindBinaryOp(head, op)) {
        return CheckBinary(ck, node, op, hint);
    }
    if (head == "~" || head == "not") {
        return CheckUnary(ck, node, hint);
    }
    if (head == "and" || head == "or") {
        return CheckLogic(ck, node);
    }
    if (head == "u8" || head == "i8" || head == "u16" || head == "i16") {
        return CheckConvert(ck, node, ResolveType(ck, head_node));
    }
    if (head == "let") {
        return CheckLet(ck, node);
    }
    if (head == "set") {
        return CheckSet(ck, node);
    }
    if (head == "if") {
        return CheckIf(ck, node, hint);
    }
    if (head == "do") {
        return CheckDo(ck, node, hint);
    }
    if (head == "while") {
        return CheckWhile(ck, node);
    }
    if (head == "for") {
        return CheckFor(ck, node);
    }
    if (head == "match") {
        return CheckMatch(ck, node, hint);
    }
    if (head == "return") {
        return CheckReturn(ck, node);
    }
    if (head == "break" || head == "continue") {
        return CheckLoopExit(ck, node, head == "break");
    }
    if (head == "at") {
        return CheckAt(ck, node);
    }
    if (head == ".") {
        return CheckDot(ck, node);
    }
    if (head == "load") {
        return CheckLoad(ck, node);
    }
    if (head == "store") {
        return CheckStore(ck, node);
    }
    if (head == "addr_of") {
        return CheckAddrOf(ck, node);
    }
    if (head == "as") {
        return CheckAs(ck, node);
    }
    if (head == "io_read") {
        return CheckIoRead(ck, node);
    }
    if (head == "io_write") {
        return CheckIoWrite(ck, node);
    }
    if (head == "fn" || head == "struct" || head == "enum" || head == "const" || head == "import" ||
        head == "macro") {
        std::string fix = head == "const" ? "; use let here" : "; move it out of this function";
        return Fail(ck, node, head + " goes at top level" + fix);
    }
    if (head == "args" || head == "returns" || head == "_" || head == "ptr" || head == "array" ||
        head == "bool" || head == "quote" || head == "quasiquote" || head == "unquote" ||
        head == "unquote_splicing") {
        return Fail(ck, node, Show(node) + " is not a form here");
    }

    std::vector<std::string> segments = SplitDotted(head);
    NameRef ref = segments.empty() ? NameRef() : LookupName(ck, segments);
    bool whole = ref.used == segments.size();
    if (ref.kind == NameKind::Function && whole) {
        return CheckCall(ck, node, ref.index);
    }
    if (ref.kind == NameKind::Struct && whole) {
        return CheckStructLit(ck, node, StructType(ck, ref.index));
    }
    if (ref.kind == NameKind::None && ref.missing_in.empty() && ref.used == 0) {
        return Fail(ck, node, UnknownName(ck, head, "function"));
    }
    if (ref.kind == NameKind::None && !ref.missing_in.empty()) {
        return Fail(ck, node, ref.missing_in + " has no " + segments[ref.used]);
    }
    return Fail(ck, node, head + " is not a function, so (" + head + " ...) is not a call");
}

} // namespace

ExprId CheckIntLiteral(Checker& ck, const Node& node, TypeId hint) {
    if (hint == kErrorType) {
        return ErrorExpr(ck, node);
    }
    if (hint < 0 || hint == kVoidType || hint == kNeverType) {
        return Fail(ck, node,
                    node.text +
                        " has no type here; a literal takes the type of what it meets, so "
                        "use it next to a typed value or declare one, as in (let n u8 " +
                        node.text + ")");
    }
    TypeKind kind = KindOf(*ck.out, hint);
    if (kind == TypeKind::Ptr) {
        return Fail(ck, node,
                    "a pointer is not a number; write (as " + ShowType(ck, hint) + " " + node.text +
                        ")");
    }
    if (kind == TypeKind::Bool) {
        return Fail(ck, node, node.text + " is not a bool; write true or false");
    }
    if (kind == TypeKind::Enum) {
        return Fail(ck, node,
                    node.text + " is not a " + ShowType(ck, hint) + "; name a member, as in " +
                        ShowType(ck, hint) + "." +
                        ck.out->enums[static_cast<std::size_t>(TypeOf(*ck.out, hint).decl)]
                            .members[0]
                            .name);
    }
    if (!IsIntegerKind(kind)) {
        return Fail(ck, node,
                    node.text + " is a number; a " + ShowType(ck, hint) + " is needed here");
    }
    if (!LiteralFits(kind, node.value)) {
        std::int64_t low = 0;
        std::int64_t high = 0;
        LiteralRange(kind, low, high);
        return Fail(ck, node,
                    node.text + " does not fit in " + ShowType(ck, hint) + " (" +
                        std::to_string(low) + " to " + std::to_string(high) + ")");
    }
    return MakeLiteral(ck, node, hint, MaskToKind(kind, static_cast<std::uint32_t>(node.value)));
}

ExprId CheckExpr(Checker& ck, const Node& node, TypeId hint) {
    switch (node.kind) {
    case NodeKind::Integer:
        return CheckIntLiteral(ck, node, hint);
    case NodeKind::Symbol:
        return CheckName(ck, node);
    case NodeKind::Keyword:
        return Fail(ck, node,
                    node.text + ": keywords only name fields in struct literals and let options");
    case NodeKind::String:
        return Fail(ck, node, "strings are not in this subset yet");
    case NodeKind::List:
        return CheckList(ck, node, hint);
    }
    return ErrorExpr(ck, node);
}

ExprId CheckStatement(Checker& ck, const Node& node) {
    if (IsBareLiteral(node)) {
        return Fail(ck, node, node.text + " on its own does nothing; remove it");
    }
    return CheckExpr(ck, node, -1);
}

bool RequireType(Checker& ck, const Node& node, ExprId expr, TypeId want, const std::string& what) {
    TypeId got = ExprType(ck, expr);
    if (got == want || got == kNeverType || got == kErrorType || want == kErrorType) {
        return true;
    }
    if (got == kVoidType) {
        Err(ck, node,
            what + " needs a " + ShowType(ck, want) + " but " + Show(node) + " gives no value");
        return false;
    }
    std::string fix;
    TypeKind gk = KindOf(*ck.out, got);
    TypeKind wk = KindOf(*ck.out, want);
    if (IsIntegerKind(gk) && IsIntegerKind(wk)) {
        fix = "; write (" + ShowType(ck, want) + " " + Show(node) + ")";
    } else if (gk == TypeKind::Ptr && want == kU16Type) {
        fix = "; write (as u16 " + Show(node) + ")";
    } else if (IsIntegerKind(gk) && want == kBoolType) {
        fix = "; write (!= " + Show(node) + " 0)";
    } else if (got == kU16Type && wk == TypeKind::Ptr) {
        fix = "; write (as " + ShowType(ck, want) + " " + Show(node) + ")";
    }
    Err(ck, node,
        what + " needs a " + ShowType(ck, want) + " but " + Show(node) + " is " +
            ShowType(ck, got) + fix);
    return false;
}

bool IsPlaceKind(ExprKind kind) {
    return kind == ExprKind::VarRef || kind == ExprKind::Index || kind == ExprKind::Field;
}

bool RequireStructSource(Checker& ck, const Node& node, ExprId expr) {
    ExprKind kind = ExprAt(ck, expr).kind;
    if (kind == ExprKind::StructLit || kind == ExprKind::Load || IsPlaceKind(kind) ||
        ExprType(ck, expr) == kErrorType) {
        return true;
    }
    Err(ck, node,
        "a struct value comes from a struct literal, a variable, (at ...), a field or (load p); "
        "set each case on its own instead, as in (if c (set s a) (set s b))");
    return false;
}

} // namespace tisple
