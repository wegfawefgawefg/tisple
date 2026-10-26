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
    std::size_t given = form.items.size() - 3;
    if (given != array.count) {
        Err(ck, form,
            cd.name + " is " + ShowType(ck, type) + " but has " + std::to_string(given) +
                " values; give exactly " + std::to_string(array.count));
        return false;
    }
    std::uint32_t elem_size = TypeOf(*ck.out, elem).size;
    std::vector<std::uint8_t> bytes(array.size, 0);
    bool ok = true;
    for (std::size_t i = 0; i < given; i++) {
        const Node& item = form.items[3 + i];
        ExprId value = CheckExpr(ck, item, elem);
        std::uint16_t raw = 0;
        if (!RequireType(ck, item, value, elem, "an element of " + cd.name) ||
            !FoldExpr(ck, value, raw)) {
            ok = false;
            continue;
        }
        PutBytes(bytes, static_cast<std::uint32_t>(i) * elem_size, elem_size, raw);
    }
    Var var;
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

// form is the const without its placement options
bool FoldConstForm(Checker& ck, ConstDecl& cd, const Node& form) {
    const std::vector<Node>& items = form.items;
    std::string fix = "write (const " + cd.name + " u8 0)";
    if (items.size() == 2) {
        Err(ck, form, "const " + cd.name + " needs a value; " + fix);
        return false;
    }
    ExprId value = -1;
    TypeId type = kErrorType;
    if (items.size() == 3) {
        if (IsBareLiteral(items[2])) {
            Err(ck, items[2],
                "(const " + cd.name + " " + items[2].text +
                    "): the literal has no type; write (const " + cd.name + " u8 " + items[2].text +
                    ")");
            return false;
        }
        TypeId named = TryResolveType(ck, items[2]);
        if (named > kErrorType && KindOf(*ck.out, named) == TypeKind::Array) {
            return FoldConstArray(ck, cd, form, named);
        }
        if (named >= 0) {
            Err(ck, form, "const " + cd.name + " needs a value; " + fix);
            return false;
        }
        value = CheckExpr(ck, items[2], -1);
        type = ExprType(ck, value);
    } else {
        type = ResolveType(ck, items[2]);
        if (type != kErrorType && KindOf(*ck.out, type) == TypeKind::Array) {
            return FoldConstArray(ck, cd, form, type);
        }
        if (items.size() != 4) {
            Err(ck, form, "(const NAME type value) takes one value; " + fix);
            return false;
        }
        value = CheckExpr(ck, items[3], type);
        if (!RequireType(ck, items[3], value, type, "const " + cd.name)) {
            return false;
        }
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

bool FoldOption(Checker& ck, const Node& node, const std::string& label, std::uint32_t& out) {
    ExprId value = CheckExpr(ck, node, kU16Type);
    std::uint16_t raw = 0;
    if (!RequireType(ck, node, value, kU16Type, label)) {
        return false;
    }
    if (!FoldExpr(ck, value, raw)) {
        Err(ck, node, label + " needs a constant number, like 0x6000 or 256");
        return false;
    }
    out = raw;
    return true;
}

void ReadOption(Checker& ck, Var& var, const Node& key, const Node* value) {
    const std::string& name = key.text;
    if (name == ":in") {
        Err(ck, key, ":in (named regions) is not in this subset yet; remove it");
        return;
    }
    if (name != ":address" && name != ":align" && name != ":boundary") {
        Err(ck, key, name + " is not a let option; use :address, :align or :boundary");
        return;
    }
    if (!value) {
        Err(ck, key, name + " needs a value; write " + name + " 256");
        return;
    }
    std::uint32_t number = 0;
    if (!FoldOption(ck, *value, name, number)) {
        return;
    }
    Placement& place = var.placement;
    bool repeated = (name == ":address" && place.has_address) ||
                    (name == ":align" && place.align != 0) ||
                    (name == ":boundary" && place.boundary != 0);
    if (repeated) {
        Err(ck, key, name + " is given twice; keep one");
        return;
    }
    if (name == ":address") {
        place.has_address = true;
        place.address = static_cast<std::uint16_t>(number);
        return;
    }
    if (number == 0) {
        Err(ck, *value, name + " 0 means nothing; use 1 or more, like " + name + " 256");
        return;
    }
    if (name == ":align") {
        place.align = number;
    } else {
        place.boundary = number;
    }
}

// placement options come last, as on let: (const T (array u8 3) 1 2 3 :align 256)
const Node* SplitOptions(Checker& ck, const Node& form, Node& trimmed, Var& holder) {
    std::size_t first = form.items.size();
    for (std::size_t i = 2; i < form.items.size(); i++) {
        if (form.items[i].kind == NodeKind::Keyword) {
            first = i;
            break;
        }
    }
    trimmed = form;
    trimmed.items.resize(first);
    for (std::size_t i = first; i < form.items.size(); i += 2) {
        const Node& key = form.items[i];
        if (key.kind != NodeKind::Keyword) {
            Err(ck, key, "placement options come last, after every value");
            continue;
        }
        const Node* value = i + 1 < form.items.size() ? &form.items[i + 1] : nullptr;
        ReadOption(ck, holder, key, value);
    }
    return first < form.items.size() ? &form.items[first] : nullptr;
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
    if (ok && done.var >= 0) {
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
        ReadOption(ck, var, item, value);
        i++;
    }

    TypeId type = -1;
    const Node* value_node = nullptr;
    if (positional.empty()) {
        Err(ck, form, "let " + name + " needs a type or a value; write (let " + name + " u8 0)");
    } else if (positional.size() > 2) {
        Err(ck, form, "(let name type value) takes at most a type and a value");
    } else if (positional.size() == 2) {
        type = ResolveType(ck, *positional[0]);
        value_node = positional[1];
    } else {
        type = TryResolveType(ck, *positional[0]);
        value_node = type >= 0 ? nullptr : positional[0];
    }

    if (value_node) {
        bool is_array = type > kErrorType && KindOf(*ck.out, type) == TypeKind::Array;
        if (is_array) {
            Err(ck, *value_node,
                "arrays start zeroed; fill " + name +
                    " in a fn, or write (const NAME (array t n) v0 v1 ...)");
        } else if (type < 0 && IsBareLiteral(*value_node)) {
            Err(ck, *value_node,
                "(let " + name + " " + value_node->text +
                    "): the literal has no type; write (let " + name + " u8 " + value_node->text +
                    ")");
            type = kErrorType;
        } else {
            ExprId value = CheckExpr(ck, *value_node, type);
            if (type >= 0) {
                RequireType(ck, *value_node, value, type, "let " + name);
            } else {
                type = ExprType(ck, value);
            }
            if (type == kVoidType || type == kNeverType) {
                Err(ck, *value_node,
                    Show(*value_node) + " gives no value to start " + name + " with");
                type = kErrorType;
            }
            if (type > kErrorType) {
                std::vector<std::uint8_t> bytes(TypeOf(*ck.out, type).size, 0);
                std::size_t before = ck.errors->size();
                if (!FoldInit(ck, value, bytes, 0)) {
                    if (ck.errors->size() == before) {
                        Err(ck, *value_node,
                            "a top-level let starts with a constant value; set " + name +
                                " at the start of main instead");
                    }
                } else {
                    var.init = bytes;
                }
            }
        }
    }
    var.type = type < 0 ? kErrorType : type;
    ck.out->vars[static_cast<std::size_t>(index)] = var;
    RestoreContext(ck, std::move(saved));
}

} // namespace tisple
