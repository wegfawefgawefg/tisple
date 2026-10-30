#include "check/arith.hpp"
#include "check/checker.hpp"

#include <algorithm>

namespace tisple {

namespace {

bool IsConstScalar(const Checker& ck, TypeId type) {
    return type != kErrorType && IsScalarKind(KindOf(*ck.out, type));
}

// a struct or array const: its Var comes first, so the value may hold its own address
// (addr_of NODES 1), then the bytes and address slots
bool FoldConstData(Checker& ck, ConstDecl& cd, const Node& form, TypeId type) {
    Var made;
    made.name = ck.out->modules[static_cast<std::size_t>(cd.module)].name + "." + cd.name;
    made.kind = VarKind::Const;
    made.type = type;
    made.module = cd.module;
    made.loc = NodeLoc(ck, form.items[1]);
    std::int32_t var = AddVar(ck, made);
    cd.var = var;
    cd.type = type;
    std::vector<std::uint8_t> bytes(TypeOf(*ck.out, type).size, 0);
    std::vector<AddrSlot> outer_slots = std::move(ck.pending_slots);
    ck.pending_slots.clear();
    ck.folding.push_back(var);
    bool ok = FoldValue(ck, form.items[3], type, cd.name, bytes, 0);
    ck.folding.pop_back();
    Var& done = ck.out->vars[static_cast<std::size_t>(var)];
    done.init = bytes;
    done.addr_slots = std::move(ck.pending_slots);
    ck.pending_slots = std::move(outer_slots);
    return ok;
}

// a pointer const: a number, or an address the link knows ("HELLO", (addr_of T 3)), kept as
// the checked tree each use copies
bool FoldConstPointer(Checker& ck, ConstDecl& cd, const Node& node, ExprId value) {
    std::uint16_t raw = 0;
    std::size_t before = ck.errors->size();
    if (FoldExpr(ck, value, raw)) {
        cd.value = raw;
        return true;
    }
    AddrSlot slot;
    if (ck.errors->size() == before && FoldAddress(ck, value, slot)) {
        cd.address = value;
        return true;
    }
    if (ck.errors->size() == before) {
        Err(ck, node,
            "the value of const " + cd.name +
                " is not constant; a pointer const is a number, a string or (addr_of x) of a "
                "top-level let or a const");
    }
    return false;
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
    TypeKind kind = type == kErrorType ? TypeKind::Error : KindOf(*ck.out, type);
    bool is_array = kind == TypeKind::Array;
    if (items.size() != 4) {
        Err(ck, form,
            is_array ? "a value is one form; " + OldTableFix(cd, form)
                     : "(const NAME type value) takes one value; " + fix);
        return false;
    }
    if (type == kErrorType) {
        return false;
    }
    if (is_array || kind == TypeKind::Struct) {
        return FoldConstData(ck, cd, form, type);
    }
    if (items[3].kind == NodeKind::String && type != PtrType(ck, kU8Type)) {
        Err(ck, items[3],
            "a string is a (ptr u8); write (const " + cd.name + " (ptr u8) " + Show(items[3]) +
                ")");
        return false;
    }
    if (kind == TypeKind::Fn) {
        Err(ck, form,
            "a function is named directly, not through a const; write " + Show(items[3]) +
                " where " + cd.name + " is used");
        return false;
    }
    value = CheckExpr(ck, items[3], type);
    if (!RequireType(ck, items[3], value, type, "const " + cd.name)) {
        return false;
    }
    if (!IsConstScalar(ck, type)) {
        Err(ck, form, "a const of type " + ShowType(ck, type) + " is not data; use a scalar type");
        return false;
    }
    cd.type = type;
    if (kind == TypeKind::Ptr) {
        return FoldConstPointer(ck, cd, items[3], value);
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
        if (holder.placement.on_disk || holder.placement.interleave != 0) {
            ok = MoveToDisk(ck, done, *option);
        }
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
    case ExprKind::Index:
    case ExprKind::Field: {
        std::int32_t var = -1;
        std::uint32_t offset = 0;
        const Type& type = TypeOf(*ck.out, e.type);
        if (!IsScalarKind(type.kind) || !ConstPlace(*ck.out, id, var, offset)) {
            return false;
        }
        if (std::find(ck.folding.begin(), ck.folding.end(), var) != ck.folding.end()) {
            AddError(*ck.errors, e.loc,
                     DataName(ck, var) + " reads its own value while it is being built; only "
                                         "its address, (addr_of ...), may be used in it");
            return false;
        }
        return InitBits(ck.out->vars[static_cast<std::size_t>(var)], offset, type.size, value);
    }
    default:
        return false;
    }
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

namespace {

// a top-level let's value into var's bytes and address slots; its type is already in the
// program, so the value may hold the let's own address
void FoldGlobalValue(Checker& ck, Var& var, const Node& value_node, const std::string& name) {
    TypeId type = var.type;
    std::vector<std::uint8_t> bytes(TypeOf(*ck.out, type).size, 0);
    std::vector<AddrSlot> outer_slots = std::move(ck.pending_slots);
    ck.pending_slots.clear();
    bool folded = false;
    TypeKind kind = KindOf(*ck.out, type);
    if (kind == TypeKind::Array || kind == TypeKind::Struct) {
        folded = FoldValue(ck, value_node, type, name, bytes, 0);
    } else {
        ExprId value = CheckExpr(ck, value_node, type);
        std::size_t before = ck.errors->size();
        folded = RequireType(ck, value_node, value, type, "let " + name) &&
                 FoldScalarValue(ck, value, type, bytes, 0);
        if (!folded && ck.errors->size() == before) {
            Err(ck, value_node,
                "a top-level let starts with a constant value; set " + name +
                    " at the start of main instead");
        }
    }
    if (folded) {
        var.init = bytes;
        var.addr_slots = std::move(ck.pending_slots);
    }
    ck.pending_slots = std::move(outer_slots);
}

} // namespace

void EnsureGlobal(Checker& ck, std::int32_t index) {
    std::size_t i = static_cast<std::size_t>(index);
    if (i < ck.global_forms.size() && ck.global_forms[i] != nullptr &&
        i < ck.global_states.size() && ck.global_states[i] == Progress::Pending) {
        ResolveGlobal(ck, index);
    }
}

void ResolveGlobal(Checker& ck, std::int32_t index) {
    ck.global_states[static_cast<std::size_t>(index)] = Progress::Busy;
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
        var.type = type;
        ck.out->vars[static_cast<std::size_t>(index)].type = type;
        FoldGlobalValue(ck, var, *value_node, name);
    }
    var.type = type < 0 ? kErrorType : type;
    CheckCallableName(ck, form.items[1], var.type, "let");
    CheckRegionOptions(ck, form, var, value_node != nullptr);
    // its value may have taken its own address
    const Var& now = ck.out->vars[static_cast<std::size_t>(index)];
    var.address_taken = var.address_taken || now.address_taken;
    ck.out->vars[static_cast<std::size_t>(index)] = var;
    ck.global_states[static_cast<std::size_t>(index)] = Progress::Done;
    RestoreContext(ck, std::move(saved));
}

} // namespace tisple
