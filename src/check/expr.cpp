#include "check/arith.hpp"
#include "check/checker.hpp"

namespace tisple {

namespace {

ExprId MakeLiteral(Checker& ck, const Node& node, TypeId type, std::uint16_t raw) {
    ExprId id = AddExpr(ck, ExprKind::Literal, type, node);
    ExprAt(ck, id).value = raw;
    return id;
}

std::string UnknownName(Checker& ck, const Node& at, const std::string& text, const char* what) {
    // a name some import has is the likely fix
    const ModuleScope& scope = ck.modules[static_cast<std::size_t>(NameModule(ck, at))];
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
    NameRef ref = LookupName(ck, segments, node);
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
        return Fail(ck, node, UnknownName(ck, node, node.text, "name"));
    case NameKind::Module:
        return Fail(ck, node,
                    first + " is a module; name one of its members, as in " + first + ".x");
    case NameKind::Function:
        return Fail(ck, node, node.text + " is a function; call it: (" + node.text + " ...)");
    case NameKind::Struct:
        return Fail(ck, node,
                    node.text + " is a type; build a value with (make " + node.text +
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
        if (cd.string) {
            value = StringPointer(ck, node, cd.var);
        } else if (cd.var >= 0) {
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
        NoteField(ck, node, i, value);
    }
    return value;
}

// (u8 x), (Sprite :x 1 ...): a type never heads a form
std::string TypeHeadFix(const Node& node) {
    std::string rest;
    for (std::size_t i = 1; i < node.items.size(); i++) {
        rest += " " + Show(node.items[i]);
    }
    return rest;
}

ExprId CheckList(Checker& ck, const Node& node, TypeId hint, bool used) {
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
        return Fail(ck, node,
                    "a type never heads a form; to convert write (as " + head + TypeHeadFix(node) +
                        ")");
    }
    if (head == "let") {
        return CheckLet(ck, node);
    }
    if (head == "set") {
        return CheckSet(ck, node);
    }
    if (head == "if") {
        return CheckIf(ck, node, hint, used);
    }
    if (head == "do") {
        return CheckDo(ck, node, hint, used);
    }
    if (head == "while") {
        return CheckWhile(ck, node);
    }
    if (head == "for") {
        return CheckFor(ck, node);
    }
    if (head == "match") {
        return CheckMatch(ck, node, hint, used);
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
    if (head == "make") {
        return CheckMake(ck, node);
    }
    if (head == "io_read") {
        return CheckIoRead(ck, node);
    }
    if (head == "io_write") {
        return CheckIoWrite(ck, node);
    }
    if (head == "jsr") {
        return CheckJsr(ck, node);
    }
    if (head == "wait_cycles") {
        return CheckWaitCycles(ck, node);
    }
    if (head == "fn" || head == "struct" || head == "enum" || head == "const" || head == "import" ||
        head == "macro") {
        std::string fix = head == "const" ? "; use let here" : "; move it out of this function";
        return Fail(ck, node, head + " goes at top level" + fix);
    }
    if (head == "ptr" || head == "array" || head == "bool" || head == "void") {
        return Fail(ck, node,
                    Show(node) + ": a type never heads a form; convert with (as t x), build with "
                                 "(make t ...)");
    }
    if (head == "_" || head == "quote" || head == "quasiquote" || head == "unquote" ||
        head == "unquote_splicing") {
        return Fail(ck, node, Show(node) + " is not a form here");
    }

    std::vector<std::string> segments = SplitDotted(head);
    NameRef ref = segments.empty() ? NameRef() : LookupName(ck, segments, head_node);
    bool whole = ref.used == segments.size();
    if (ref.kind == NameKind::Function && whole) {
        return CheckCall(ck, node, ref.index);
    }
    if (ref.kind == NameKind::Struct && whole) {
        return Fail(ck, node,
                    "a type never heads a form; to build a " + head + " write (make " + head +
                        TypeHeadFix(node) + ")");
    }
    if (ref.kind == NameKind::Enum && whole) {
        return Fail(ck, node,
                    "nothing converts to an enum; name a member, as in " + head + "." +
                        ck.out->enums[static_cast<std::size_t>(ref.index)].members[0].name +
                        ", or match on the number");
    }
    if (ref.kind == NameKind::None && ref.missing_in.empty() && ref.used == 0) {
        return Fail(ck, node, UnknownName(ck, head_node, head, "function"));
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
        return CheckString(ck, node);
    case NodeKind::List:
        return CheckList(ck, node, hint, true);
    }
    return ErrorExpr(ck, node);
}

ExprId CheckStatement(Checker& ck, const Node& node) {
    if (IsBareLiteral(node) || node.kind == NodeKind::String) {
        return Fail(ck, node, node.text + " on its own does nothing; remove it");
    }
    if (node.kind == NodeKind::List) {
        return CheckList(ck, node, -1, false);
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
    if ((IsIntegerKind(gk) || gk == TypeKind::Enum) && IsIntegerKind(wk)) {
        fix = "; write (as " + ShowType(ck, want) + " " + Show(node) + ")";
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
    if (kind == ExprKind::StructLit || kind == ExprKind::Load || kind == ExprKind::LoadField ||
        IsPlaceKind(kind) || ExprType(ck, expr) == kErrorType) {
        return true;
    }
    Err(ck, node,
        "a struct value comes from (make ...), a variable, (at ...), a field or a load; "
        "set each case on its own instead, as in (if c (set s a) (set s b))");
    return false;
}

} // namespace tisple
