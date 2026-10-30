#include "check/arith.hpp"
#include "check/checker.hpp"

namespace tisple {

namespace {

void PutBytes(std::vector<std::uint8_t>& bytes, std::uint32_t offset, std::uint32_t size,
              std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFF);
    if (size == 2) {
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    }
}

bool IsConstScalar(const Checker& ck, TypeId type) {
    return type != kErrorType && IsScalarKind(KindOf(*ck.out, type));
}

bool FoldConstArray(Checker& ck, ConstDecl& cd, const Node& form, TypeId type) {
    const Type& array = TypeOf(*ck.out, type);
    TypeId elem = array.elem;
    if (!IsConstScalar(ck, elem)) {
        Err(ck, form.items[2],
            "a const array holds numbers, bools, enum members or pointers; use a top-level let "
            "for an array of " +
                ShowType(ck, elem));
        return false;
    }
    std::vector<std::uint8_t> bytes(array.size, 0);
    std::vector<FnSlot> outer_slots = std::move(ck.pending_slots);
    ck.pending_slots.clear();
    bool ok = FoldArrayLiteral(ck, form.items[3], type, cd.name, bytes, 0);
    Var var;
    var.fn_slots = std::move(ck.pending_slots);
    ck.pending_slots = std::move(outer_slots);
    var.name = ck.out->modules[static_cast<std::size_t>(cd.module)].name + "." + cd.name;
    var.kind = VarKind::ConstArray;
    var.type = type;
    var.module = cd.module;
    var.loc = NodeLoc(ck, form.items[1]);
    var.init = bytes;
    cd.var = AddVar(ck, var);
    cd.type = type;
    return ok;
}

// (const GREETING (ptr u8) "HELLO"): the string's table, read as a pointer to it
bool FoldConstString(Checker& ck, ConstDecl& cd, const Node& value, TypeId type) {
    if (type == kErrorType) {
        return false;
    }
    if (type != PtrType(ck, kU8Type)) {
        Err(ck, value,
            "a string is a (ptr u8); write (const " + cd.name + " (ptr u8) " + Show(value) + ")");
        return false;
    }
    cd.var = StringTable(ck, value);
    cd.type = type;
    cd.string = true;
    return cd.var >= 0;
}

// (const T (array u8 3) 1 2 3): the old table spelling, values straight after the type
std::string OldTableFix(const ConstDecl& cd, const Node& form) {
    std::string type = Show(form.items[2]);
    std::string values;
    for (std::size_t i = 3; i < form.items.size() && i < 5; i++) {
        values += " " + Show(form.items[i]);
    }
    return "write (const " + cd.name + " " + type + " (make " + type + values + " ...))";
}

// form is the const without its placement options: (const NAME type value)
bool FoldConstForm(Checker& ck, ConstDecl& cd, const Node& form) {
    const std::vector<Node>& items = form.items;
    std::string fix = "write (const " + cd.name + " u8 0)";
    if (items.size() == 2) {
        Err(ck, form, "const " + cd.name + " needs a type and a value; " + fix);
        return false;
    }
    ExprId value = -1;
    TypeId type = kErrorType;
    if (items.size() == 3) {
        if (IsBareLiteral(items[2])) {
            Err(ck, items[2],
                "(const " + cd.name + " " + items[2].text +
                    "): const always states the type; write (const " + cd.name + " u8 " +
                    items[2].text + ")");
            return false;
        }
        TypeId named = TryResolveType(ck, items[2]);
        if (named >= 0) {
            std::string shown = "value";
            if (named > kErrorType && KindOf(*ck.out, named) == TypeKind::Array) {
                shown = "(make " + ShowType(ck, named) + " v0 v1 ...)";
            }
            Err(ck, form,
                "const " + cd.name + " needs a value; write (const " + cd.name + " " +
                    Show(items[2]) + " " + shown + ")");
            return false;
        }
        value = CheckExpr(ck, items[2], -1);
        type = ExprType(ck, value);
        if (type > kErrorType) {
            Err(ck, form,
                "const always states the type; write (const " + cd.name + " " + ShowType(ck, type) +
                    " " + Show(items[2]) + ")");
        }
        return false;
    }
    type = ResolveType(ck, items[2]);
    bool is_array = type != kErrorType && KindOf(*ck.out, type) == TypeKind::Array;
    if (items.size() != 4) {
        Err(ck, form,
            is_array ? "a value is one form; " + OldTableFix(cd, form)
                     : "(const NAME type value) takes one value; " + fix);
        return false;
    }
    if (is_array) {
        return FoldConstArray(ck, cd, form, type);
    }
    if (items[3].kind == NodeKind::String) {
        return FoldConstString(ck, cd, items[3], type);
    }
    value = CheckExpr(ck, items[3], type);
    if (!RequireType(ck, items[3], value, type, "const " + cd.name)) {
        return false;
    }
    if (type == kErrorType) {
        return false;
    }
    if (!IsConstScalar(ck, type)) {
        Err(ck, form,
            "a const holds a number, bool, enum member or pointer, or an array of them; use a "
            "top-level let for " +
                ShowType(ck, type));
        return false;
    }
    if (KindOf(*ck.out, type) == TypeKind::Fn) {
        Err(ck, form,
            "a function is named directly, not through a const; write " + Show(items[3]) +
                " where " + cd.name + " is used");
        return false;
    }
    std::size_t before = ck.errors->size();
    std::uint16_t raw = 0;
    if (!FoldExpr(ck, value, raw)) {
        if (ck.errors->size() == before) {
            Err(ck, items.back(),
                "the value of const " + cd.name +
                    " is not constant; use numbers, other constants and operators");
        }
        return false;
    }
    cd.type = type;
    cd.value = raw;
    return true;
}

} // namespace

void EvalConst(Checker& ck, std::int32_t index) {
    ConstDecl& cd = ck.consts[static_cast<std::size_t>(index)];
    if (cd.state == Progress::Done || cd.state == Progress::Failed) {
        return;
    }
    if (cd.state == Progress::Busy) {
        Context saved = SwitchContext(ck, cd.module, -1);
        Err(ck, *cd.form,
            "constant " + cd.name + " depends on itself; give it a value that does not use " +
                cd.name);
        RestoreContext(ck, std::move(saved));
        cd.state = Progress::Failed;
        return;
    }
    cd.state = Progress::Busy;
    Context saved = SwitchContext(ck, cd.module, -1);
    Node trimmed;
    Var holder;
    const Node* option = SplitOptions(ck, *cd.form, trimmed, holder);
    bool ok = FoldConstForm(ck, cd, trimmed);
    ConstDecl& done = ck.consts[static_cast<std::size_t>(index)];
    if (ok && done.var >= 0 && (holder.placement.in_aux || holder.placement.reserve)) {
        Err(ck, *option,
            std::string(holder.placement.in_aux ? ":in aux" : ":reserve") +
                " is for top-level lets; make " + done.name + " a let");
        ok = false;
    } else if (ok && done.var >= 0) {
        ck.out->vars[static_cast<std::size_t>(done.var)].placement = holder.placement;
    } else if (ok && option) {
        Err(ck, *option,
            "placement options are for const tables and top-level lets; " + done.name +
                " is a single value with no address");
        ok = false;
    }
    RestoreContext(ck, std::move(saved));
    if (done.state == Progress::Busy) {
        done.state = ok ? Progress::Done : Progress::Failed;
    }
}

bool FoldExpr(Checker& ck, ExprId id, std::uint16_t& value) {
    const Expr e = ExprAt(ck, id);
    std::uint16_t a = 0;
    std::uint16_t b = 0;
    switch (e.kind) {
    case ExprKind::Literal:
        value = static_cast<std::uint16_t>(e.value);
        return e.type != kErrorType;
    case ExprKind::Unary:
        if (!FoldExpr(ck, e.a, a)) {
            return false;
        }
        value = EvalUnaryOp(e.op, KindOf(*ck.out, e.type), a);
        return true;
    case ExprKind::Binary: {
        if (!FoldExpr(ck, e.a, a) || !FoldExpr(ck, e.b, b)) {
            return false;
        }
        bool div_by_zero = false;
        value = EvalBinaryOp(e.op, KindOf(*ck.out, ExprType(ck, e.a)), a, b, div_by_zero);
        if (div_by_zero) {
            AddError(*ck.errors, e.loc, "division by zero in a constant");
            return false;
        }
        return true;
    }
    case ExprKind::And:
    case ExprKind::Or:
        if (!FoldExpr(ck, e.a, a) || !FoldExpr(ck, e.b, b)) {
            return false;
        }
        value = e.kind == ExprKind::And ? (a && b ? 1 : 0) : (a || b ? 1 : 0);
        return true;
    case ExprKind::Convert:
        if (!FoldExpr(ck, e.a, a)) {
            return false;
        }
        value = ConvertBits(KindOf(*ck.out, ExprType(ck, e.a)), KindOf(*ck.out, e.type), a);
        return true;
    case ExprKind::Reinterpret:
        return FoldExpr(ck, e.a, value);
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub: {
        if (!FoldExpr(ck, e.a, a) || !FoldExpr(ck, e.b, b)) {
            return false;
        }
        std::uint32_t bytes = static_cast<std::uint32_t>(b) * e.value;
        std::uint32_t sum = e.kind == ExprKind::PtrAdd ? a + bytes : a - bytes;
        value = static_cast<std::uint16_t>(sum & 0xFFFF);
        return true;
    }
    case ExprKind::If:
        if (e.c < 0 || !FoldExpr(ck, e.a, a)) {
            return false;
        }
        return FoldExpr(ck, a != 0 ? e.b : e.c, value);
    case ExprKind::Index: {
        const Expr& base = ExprAt(ck, e.a);
        if (base.kind != ExprKind::VarRef || !FoldExpr(ck, e.b, b)) {
            return false;
        }
        const Var& var = ck.out->vars[static_cast<std::size_t>(base.ref)];
        std::uint32_t size = TypeOf(*ck.out, e.type).size;
        std::uint32_t offset = static_cast<std::uint32_t>(b) * size;
        if (var.kind != VarKind::ConstArray || offset + size > var.init.size()) {
            return false;
        }
        value = var.init[offset];
        if (size == 2) {
            value = static_cast<std::uint16_t>(value | (var.init[offset + 1] << 8));
        }
        return true;
    }
    default:
        return false;
    }
}

bool FoldInit(Checker& ck, ExprId id, std::vector<std::uint8_t>& bytes, std::uint32_t offset) {
    const Expr e = ExprAt(ck, id);
    const Type& type = TypeOf(*ck.out, e.type);
    if (e.kind == ExprKind::StructLit) {
        const StructDecl& sd = ck.out->structs[static_cast<std::size_t>(type.decl)];
        bool ok = true;
        for (std::uint32_t i = 0; i < e.count && i < sd.fields.size(); i++) {
            ExprId field = ck.out->lists[e.first + i];
            ok = FoldInit(ck, field, bytes, offset + sd.fields[i].offset) && ok;
        }
        return ok;
    }
    if (!IsScalarKind(type.kind)) {
        return false;
    }
    std::uint16_t raw = 0;
    if (!FoldExpr(ck, id, raw)) {
        return false;
    }
    PutBytes(bytes, offset, type.size, raw);
    return true;
}

bool ConstCount(Checker& ck, const Node& node, std::uint32_t& count) {
    std::int64_t value = 0;
    if (IsBareLiteral(node)) {
        value = node.value;
    } else {
        ExprId expr = CheckExpr(ck, node, -1);
        TypeId type = ExprType(ck, expr);
        if (type == kErrorType) {
            return false;
        }
        std::uint16_t raw = 0;
        std::size_t before = ck.errors->size();
        if (!IsIntegerKind(KindOf(*ck.out, type)) || !FoldExpr(ck, expr, raw)) {
            if (ck.errors->size() == before) {
                Err(ck, node, "an array length is a constant number; write 16 or a const name");
            }
            return false;
        }
        value = SignedValue(KindOf(*ck.out, type), raw);
    }
    if (value < 1 || value > 65535) {
        Err(ck, node, "an array has 1 to 65535 elements, not " + std::to_string(value));
        return false;
    }
    count = static_cast<std::uint32_t>(value);
    return true;
}

void ResolveGlobal(Checker& ck, std::int32_t index) {
    const Node& form = *ck.global_forms[static_cast<std::size_t>(index)];
    Var var = ck.out->vars[static_cast<std::size_t>(index)];
    Context saved = SwitchContext(ck, var.module, -1);
    const std::string& name = form.items[1].text;
    std::vector<const Node*> positional;
    for (std::size_t i = 2; i < form.items.size(); i++) {
        const Node& item = form.items[i];
        if (item.kind != NodeKind::Keyword) {
            positional.push_back(&item);
            continue;
        }
        const Node* value = i + 1 < form.items.size() ? &form.items[i + 1] : nullptr;
        i += static_cast<std::size_t>(ReadOption(ck, var, item, value)) - 1;
    }

    TypeId type = -1;
    const Node* value_node = nullptr;
    if (positional.empty()) {
        Err(ck, form, "let " + name + " needs a type; write (let " + name + " u8 0)");
    } else if (positional.size() > 2) {
        Err(ck, form, "(let name type value) takes one type and one value");
    } else if (positional.size() == 2) {
        type = ResolveType(ck, *positional[0]);
        value_node = positional[1];
    } else {
        type = TryResolveType(ck, *positional[0]);
        value_node = type >= 0 ? nullptr : positional[0];
    }

    if (value_node && type < 0 && IsBareLiteral(*value_node)) {
        Err(ck, *value_node,
            "(let " + name + " " + value_node->text + "): let always states the type; write (let " +
                name + " u8 " + value_node->text + ")");
        type = kErrorType;
    } else if (value_node && type < 0) {
        // the old (let name value): say what to write
        ExprId value = CheckExpr(ck, *value_node, -1);
        type = ExprType(ck, value);
        if (type > kErrorType && type != kVoidType && type != kNeverType) {
            Err(ck, *value_node,
                "let always states the type; write (let " + name + " " + ShowType(ck, type) + " " +
                    Show(*value_node) + ")");
        } else {
            type = kErrorType;
        }
    } else if (value_node && type > kErrorType) {
        std::vector<std::uint8_t> bytes(TypeOf(*ck.out, type).size, 0);
        bool folded = false;
        if (KindOf(*ck.out, type) == TypeKind::Array) {
            std::vector<FnSlot> outer_slots = std::move(ck.pending_slots);
            ck.pending_slots.clear();
            folded = FoldArrayLiteral(ck, *value_node, type, name, bytes, 0);
            if (!ck.pending_slots.empty()) {
                Err(ck, *value_node,
                    "only a const table starts with functions; make " + name +
                        " a const, or set its elements in code");
                folded = false;
            }
            ck.pending_slots = std::move(outer_slots);
        } else {
            ExprId value = CheckExpr(ck, *value_node, type);
            std::size_t before = ck.errors->size();
            folded = RequireType(ck, *value_node, value, type, "let " + name) &&
                     FoldInit(ck, value, bytes, 0);
            if (!folded && ck.errors->size() == before) {
                Err(ck, *value_node,
                    "a top-level let starts with a constant value; set " + name +
                        " at the start of main instead");
            }
        }
        if (folded) {
            var.init = bytes;
        }
    }
    var.type = type < 0 ? kErrorType : type;
    CheckRegionOptions(ck, form, var, value_node != nullptr);
    ck.out->vars[static_cast<std::size_t>(index)] = var;
    RestoreContext(ck, std::move(saved));
}

} // namespace tisple
