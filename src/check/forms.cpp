#include "check/checker.hpp"

namespace tisple {

namespace {

// the smallest type a fix can suggest for (for i start end ...)
const char* LoopTypeFor(std::int64_t start, std::int64_t end) {
    if (start < 0) {
        return start >= -128 && end <= 128 ? "i8" : "i16";
    }
    return end <= 256 ? "u8" : "u16";
}

const std::string& FunctionName(const Checker& ck) {
    return ck.function_forms[static_cast<std::size_t>(ck.at.function)]->items[1].text;
}

std::int32_t NewLocal(Checker& ck, const Node& name, TypeId type, bool loop_var) {
    Var var;
    var.name = name.text;
    var.kind = VarKind::Local;
    var.type = type;
    var.module = ck.at.module;
    var.function = ck.at.function;
    var.loc = NodeLoc(ck, name);
    var.loop_var = loop_var;
    std::int32_t id = AddVar(ck, var);
    Note(ck, name, 0, NameUseKind::Var, id, -1, true);
    ck.out->functions[static_cast<std::size_t>(ck.at.function)].locals.push_back(id);
    return id;
}

bool IsArray(const Checker& ck, TypeId type) {
    return type > kErrorType && KindOf(*ck.out, type) == TypeKind::Array;
}

bool IsStruct(const Checker& ck, TypeId type) {
    return type > kErrorType && KindOf(*ck.out, type) == TypeKind::Struct;
}

ExprId CheckStatementArm(Checker& ck, const Node& node) {
    ck.at.scopes.emplace_back();
    ExprId id = CheckStatement(ck, node);
    ck.at.scopes.pop_back();
    return id;
}

// how to give a local of this type its start value
std::string LetFix(const Checker& ck, const std::string& name, TypeId type) {
    const Type& t = TypeOf(*ck.out, type);
    if (t.kind == TypeKind::Array) {
        return "arrays have no value to start from, so make " + name + " a top-level let";
    }
    if (t.kind == TypeKind::Struct) {
        const StructDecl& sd = ck.out->structs[static_cast<std::size_t>(t.decl)];
        std::string literal = "(make " + sd.name;
        for (const Field& field : sd.fields) {
            literal += " :" + field.name + " ...";
        }
        return "write (let " + name + " " + sd.name + " " + literal + "))";
    }
    std::string zero = "0";
    if (t.kind == TypeKind::Bool) {
        zero = "false";
    } else if (t.kind == TypeKind::Enum) {
        const EnumDecl& ed = ck.out->enums[static_cast<std::size_t>(t.decl)];
        zero = ed.name + "." + ed.members[0].name;
    } else if (t.kind == TypeKind::Ptr) {
        zero = "(as " + ShowType(ck, type) + " 0)";
    }
    return "write (let " + name + " " + ShowType(ck, type) + " " + zero + ")";
}

// the start value of a let: checks it against the declared type, or infers one
ExprId CheckLetValue(Checker& ck, const Node& name, const Node& value, TypeId& type) {
    if (IsArray(ck, type)) {
        return Fail(ck, value,
                    "arrays start zeroed and are copied element by element; fill " + name.text +
                        " with a for loop");
    }
    if (type < 0 && IsBareLiteral(value)) {
        type = kErrorType;
        return Fail(ck, value,
                    "(let " + name.text + " " + value.text +
                        "): let always states the type; write (let " + name.text + " u8 " +
                        value.text + ")");
    }
    ExprId init = CheckExpr(ck, value, type);
    if (type >= 0) {
        RequireType(ck, value, init, type, "let " + name.text);
    } else {
        // the old (let name value): say what to write, then go on with the value's type
        type = ExprType(ck, init);
        if (type > kErrorType && type != kVoidType && type != kNeverType) {
            Err(ck, value,
                "let always states the type; write (let " + name.text + " " + ShowType(ck, type) +
                    " " + Show(value) + ")");
        }
    }
    if (type == kVoidType || type == kNeverType) {
        Err(ck, value, Show(value) + " gives no value to start " + name.text + " with");
        type = kErrorType;
    }
    if (IsArray(ck, type)) {
        Err(ck, value,
            "arrays are copied element by element; declare (let " + name.text + " " +
                ShowType(ck, type) + ") and fill it with a for loop");
        type = kErrorType;
    }
    if (IsStruct(ck, type)) {
        RequireStructSource(ck, value, init);
    }
    return init;
}

} // namespace

ExprId CheckBody(Checker& ck, const std::vector<Node>& items, std::size_t first, TypeId hint,
                 bool want_value, const Node& owner) {
    std::vector<ExprId> forms;
    for (std::size_t i = first; i < items.size(); i++) {
        bool last = i + 1 == items.size();
        if (last && want_value) {
            forms.push_back(CheckExpr(ck, items[i], hint));
        } else {
            forms.push_back(CheckStatement(ck, items[i]));
        }
    }
    TypeId type = kVoidType;
    if (want_value && !forms.empty()) {
        type = ExprType(ck, forms.back());
    }
    ExprId id = AddExpr(ck, ExprKind::Do, type, owner);
    ExprAt(ck, id).first = AddList(ck, forms);
    ExprAt(ck, id).count = static_cast<std::uint32_t>(forms.size());
    return id;
}

ExprId CheckLet(Checker& ck, const Node& node) {
    if (ck.at.function < 0) {
        return Fail(ck, node, "let declares a variable; it cannot be part of a constant value");
    }
    if (node.items.size() < 2 || node.items[1].kind != NodeKind::Symbol) {
        return Fail(ck, node, "let needs a name; write (let n u8 0)");
    }
    const Node& name = node.items[1];
    std::vector<const Node*> positional;
    for (std::size_t i = 2; i < node.items.size(); i++) {
        if (node.items[i].kind == NodeKind::Keyword) {
            return Fail(
                ck, node.items[i],
                "placement options (:address, :align, :boundary, :in, :reserve) are for top-level "
                "lets; "
                "move " +
                    name.text + " out of the function");
        }
        positional.push_back(&node.items[i]);
    }
    if (positional.empty()) {
        return Fail(ck, node,
                    "let " + name.text + " needs a type and a value; write (let " + name.text +
                        " u8 0)");
    }
    if (positional.size() > 2) {
        return Fail(ck, node, "(let name type value) takes one type and one value");
    }
    bool name_ok = CheckLocalName(ck, name, "local");
    TypeId type = -1;
    const Node* value = nullptr;
    if (positional.size() == 2) {
        type = ResolveType(ck, *positional[0]);
        value = positional[1];
    } else {
        type = TryResolveType(ck, *positional[0]);
        value = type >= 0 ? nullptr : positional[0];
    }
    ExprId init = -1;
    if (value) {
        init = CheckLetValue(ck, name, *value, type);
    } else if (type > kErrorType) {
        Err(ck, node, "a local let needs a value; " + LetFix(ck, name.text, type));
    }
    if (type < 0) {
        type = kErrorType;
    }
    name_ok = CheckCallableName(ck, name, type, "local") && name_ok;
    std::int32_t var = NewLocal(ck, name, type, false);
    if (name_ok) {
        ck.at.scopes.back().entries.push_back(ScopeEntry{name.text, var});
    }
    ExprId id = AddExpr(ck, ExprKind::Let, kVoidType, node);
    ExprAt(ck, id).ref = var;
    ExprAt(ck, id).a = init;
    return id;
}

ExprId CheckSet(Checker& ck, const Node& node) {
    if (node.items.size() != 3) {
        return Fail(ck, node, "set takes a place and a value; write (set x 1)");
    }
    const Node& place_node = node.items[1];
    ExprId place = CheckExpr(ck, place_node, -1);
    const Expr& expr = ExprAt(ck, place);
    TypeId type = expr.type;
    if (type == kErrorType) {
        // the value is still checked, but a bare literal in it has no type to complain about
        CheckExpr(ck, node.items[2], kErrorType);
        return ErrorExpr(ck, node);
    }
    if (expr.kind == ExprKind::Literal) {
        return Fail(ck, place_node, Show(place_node) + " is a constant; it cannot be set");
    }
    bool through_load = false;
    std::int32_t root = PlaceRoot(ck, place, through_load);
    if (through_load || expr.kind == ExprKind::Load || expr.kind == ExprKind::LoadField) {
        std::string fix = "(store p x value)";
        if (HeadName(place_node) == "load") {
            fix = "(store";
            for (std::size_t i = 1; i < place_node.items.size(); i++) {
                fix += " " + Show(place_node.items[i]);
            }
            fix += " " + Show(node.items[2]) + ")";
        }
        return Fail(ck, place_node, "set does not write through pointers; write " + fix);
    }
    if (root < 0 || !IsPlaceKind(expr.kind)) {
        return Fail(ck, place_node,
                    "set needs a variable, s.x or (at ...); " + Show(place_node) + " is not one");
    }
    const Var& var = ck.out->vars[static_cast<std::size_t>(root)];
    if (var.kind == VarKind::Const) {
        return Fail(ck, place_node, Show(place_node) + " is part of a constant; it cannot be set");
    }
    if (var.loop_var && expr.kind == ExprKind::VarRef) {
        return Fail(ck, place_node,
                    "the loop variable " + var.name +
                        " is read-only; copy it into a let to change it");
    }
    if (IsArray(ck, type)) {
        return Fail(ck, node, "arrays are copied element by element; use a for loop");
    }
    ExprId value = CheckExpr(ck, node.items[2], type);
    RequireType(ck, node.items[2], value, type, "set " + Show(place_node));
    if (IsStruct(ck, type)) {
        RequireStructSource(ck, node.items[2], value);
    }
    ExprId id = AddExpr(ck, ExprKind::Set, kVoidType, node);
    ExprAt(ck, id).a = place;
    ExprAt(ck, id).b = value;
    return id;
}

ExprId CheckIf(Checker& ck, const Node& node, TypeId hint, bool used) {
    if (node.items.size() != 3 && node.items.size() != 4) {
        return Fail(ck, node,
                    "if takes a condition and one or two arms; group forms with (do ...)");
    }
    ExprId cond = CheckExpr(ck, node.items[1], kBoolType);
    RequireType(ck, node.items[1], cond, kBoolType, "the if condition");
    ExprId id = -1;
    if (node.items.size() == 3 && used) {
        // one error for (let k u8 (if c 1)), not "1 on its own does nothing" as well
        CheckExpr(ck, node.items[2], hint < 0 ? kErrorType : hint);
        return Fail(ck, node,
                    "an if without an else gives no value; write (if " + Show(node.items[1]) + " " +
                        Show(node.items[2]) + " other)");
    }
    if (node.items.size() == 3) {
        ExprId then = CheckStatementArm(ck, node.items[2]);
        id = AddExpr(ck, ExprKind::If, kVoidType, node);
        ExprAt(ck, id).a = cond;
        ExprAt(ck, id).b = then;
        return id;
    }
    std::vector<Arm> arms = {Arm{&node.items, 2, 3, &node.items[2]},
                             Arm{&node.items, 3, 4, &node.items[3]}};
    std::vector<ExprId> out;
    TypeId type = CheckArms(ck, arms, hint, used, out, "if");
    id = AddExpr(ck, ExprKind::If, type, node);
    ExprAt(ck, id).a = cond;
    ExprAt(ck, id).b = out[0];
    ExprAt(ck, id).c = out[1];
    return id;
}

ExprId CheckDo(Checker& ck, const Node& node, TypeId hint, bool used) {
    ck.at.scopes.emplace_back();
    ExprId id = CheckBody(ck, node.items, 1, hint, used, node);
    ck.at.scopes.pop_back();
    return id;
}

ExprId CheckWhile(Checker& ck, const Node& node) {
    if (node.items.size() < 2) {
        return Fail(ck, node, "while needs a condition; write (while c body ...)");
    }
    ExprId cond = CheckExpr(ck, node.items[1], kBoolType);
    RequireType(ck, node.items[1], cond, kBoolType, "the while condition");
    ck.at.loop_depth++;
    ck.at.scopes.emplace_back();
    ExprId body = CheckBody(ck, node.items, 2, -1, false, node);
    ck.at.scopes.pop_back();
    ck.at.loop_depth--;
    ExprId id = AddExpr(ck, ExprKind::While, kVoidType, node);
    ExprAt(ck, id).a = cond;
    ExprAt(ck, id).b = body;
    return id;
}

ExprId CheckFor(Checker& ck, const Node& node) {
    const std::vector<Node>& items = node.items;
    bool typed = items.size() > 2 && TryResolveType(ck, items[2]) >= 0;
    std::size_t at = typed ? 3 : 2;
    if (items.size() < at + 2 || items[1].kind != NodeKind::Symbol) {
        return Fail(ck, node,
                    "for needs a name, a type, a start and an end; write (for i u8 0 10 body ...)");
    }
    const Node& start_node = items[at];
    const Node& end_node = items[at + 1];
    ExprId start = -1;
    ExprId end = -1;
    TypeId type = kErrorType;
    if (typed) {
        type = ResolveType(ck, items[2]);
    } else if (IsBareLiteral(start_node) && IsBareLiteral(end_node)) {
        return Fail(ck, node,
                    "for always states the type; write (for " + items[1].text + " " +
                        LoopTypeFor(start_node.value, end_node.value) + " " + start_node.text +
                        " " + end_node.text + " ...)");
    } else {
        // the old (for i start end ...): say what to write, then go on with the type found
        if (!IsBareLiteral(start_node)) {
            start = CheckExpr(ck, start_node, -1);
            type = ExprType(ck, start);
        } else {
            end = CheckExpr(ck, end_node, -1);
            type = ExprType(ck, end);
        }
        if (type > kErrorType && IsIntegerKind(KindOf(*ck.out, type))) {
            Err(ck, node,
                "for always states the type; write (for " + items[1].text + " " +
                    ShowType(ck, type) + " " + Show(start_node) + " " + Show(end_node) + " ...)");
        }
    }
    if (type != kErrorType && !IsIntegerKind(KindOf(*ck.out, type))) {
        Err(ck, node, "for counts with an integer type, not " + ShowType(ck, type));
        type = kErrorType;
    }
    if (start < 0) {
        start = CheckExpr(ck, start_node, type);
    }
    RequireType(ck, start_node, start, type, "the start of the loop");
    bool through_max = false;
    if (end < 0 && IsBareLiteral(end_node) && type != kErrorType) {
        // (for i u8 0 256 ...) runs through 255: the end may be one past the largest value
        std::int64_t past = KindBits(KindOf(*ck.out, type)) == 8 ? 256 : 65536;
        if (IsSignedKind(KindOf(*ck.out, type))) {
            past /= 2;
        }
        through_max = end_node.value == past;
    }
    bool end_too_big = end < 0 && IsBareLiteral(end_node) && type > kErrorType &&
                       KindBits(KindOf(*ck.out, type)) == 8 &&
                       !LiteralFits(KindOf(*ck.out, type), end_node.value) && !through_max;
    if (end_too_big) {
        // (for i u8 0 300 ...): the loop needs the wider type
        const char* wider = IsSignedKind(KindOf(*ck.out, type)) ? "i16" : "u16";
        end = Fail(ck, end_node,
                   end_node.text + " does not fit in " + ShowType(ck, type) +
                       ", the loop's type; write (for " + items[1].text + " " + wider + " " +
                       Show(start_node) + " " + end_node.text + " ...)");
    }
    if (!through_max) {
        if (end < 0) {
            end = CheckExpr(ck, end_node, type);
        }
        RequireType(ck, end_node, end, type, "the end of the loop");
    } else {
        end = -1;
    }

    ck.at.scopes.emplace_back();
    std::int32_t var = NewLocal(ck, items[1], type, true);
    if (CheckLocalName(ck, items[1], "loop variable")) {
        ck.at.scopes.back().entries.push_back(ScopeEntry{items[1].text, var});
    }
    ck.at.loop_depth++;
    ExprId body = CheckBody(ck, items, at + 2, -1, false, node);
    ck.at.loop_depth--;
    ck.at.scopes.pop_back();
    ExprId id = AddExpr(ck, ExprKind::For, kVoidType, node);
    Expr& expr = ExprAt(ck, id);
    expr.ref = var;
    expr.a = start;
    expr.b = end;
    expr.c = body;
    return id;
}

ExprId CheckReturn(Checker& ck, const Node& node) {
    if (ck.at.function < 0) {
        return Fail(ck, node, "return is only inside a fn");
    }
    TypeId ret = ck.out->functions[static_cast<std::size_t>(ck.at.function)].ret;
    const std::string& name = FunctionName(ck);
    ExprId value = -1;
    if (node.items.size() > 2) {
        return Fail(ck, node, "return takes at most one value");
    }
    if (node.items.size() == 1 && ret != kVoidType && ret != kErrorType) {
        return Fail(ck, node, name + " returns " + ShowType(ck, ret) + "; write (return value)");
    }
    if (node.items.size() == 2) {
        if (ret == kVoidType) {
            return Fail(ck, node,
                        name + " returns void, so it gives no value; write (return) or give " +
                            name + " a return type");
        }
        value = CheckExpr(ck, node.items[1], ret);
        RequireType(ck, node.items[1], value, ret, "the return value of " + name);
    }
    ExprId id = AddExpr(ck, ExprKind::Return, kNeverType, node);
    ExprAt(ck, id).a = value;
    return LeaveAux(ck, node, id);
}

ExprId CheckLoopExit(Checker& ck, const Node& node, bool is_break) {
    const char* name = is_break ? "break" : "continue";
    if (node.items.size() != 1) {
        return Fail(ck, node, std::string(name) + " takes nothing; write (" + name + ")");
    }
    if (ck.at.loop_depth == 0) {
        return Fail(ck, node, std::string(name) + " is only inside while or for");
    }
    ExprId id = AddExpr(ck, is_break ? ExprKind::Break : ExprKind::Continue, kNeverType, node);
    return LeaveAux(ck, node, id);
}

} // namespace tisple
