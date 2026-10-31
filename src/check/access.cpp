#include "check/checker.hpp"

namespace tisple {

namespace {

bool IsPlaceLike(ExprKind kind) {
    return IsPlaceKind(kind) || kind == ExprKind::Load || kind == ExprKind::LoadField;
}

TypeKind Kind(const Checker& ck, TypeId type) {
    return KindOf(*ck.out, type);
}

std::string FieldList(const StructDecl& sd) {
    std::string text;
    for (const Field& field : sd.fields) {
        text += (text.empty() ? "" : " ") + field.name;
    }
    return text;
}

void AddCallee(Checker& ck, std::int32_t function) {
    if (ck.at.function < 0) {
        return;
    }
    std::vector<std::int32_t>& callees =
        ck.out->functions[static_cast<std::size_t>(ck.at.function)].callees;
    for (std::int32_t callee : callees) {
        if (callee == function) {
            return;
        }
    }
    callees.push_back(function);
}

std::string ParamText(const Checker& ck, const Function& fn) {
    std::string text;
    for (std::int32_t param : fn.params) {
        const Var& var = ck.out->vars[static_cast<std::size_t>(param)];
        text += (text.empty() ? "" : " ") + var.name;
    }
    return text;
}

} // namespace

std::int32_t PlaceRoot(Checker& ck, ExprId place, bool& through_load) {
    through_load = false;
    while (true) {
        const Expr& expr = ExprAt(ck, place);
        if (expr.kind == ExprKind::VarRef) {
            return expr.ref;
        }
        if (expr.kind == ExprKind::Load || expr.kind == ExprKind::LoadField) {
            through_load = true;
            return -1;
        }
        if (expr.kind != ExprKind::Index && expr.kind != ExprKind::Field) {
            return -1;
        }
        place = expr.a;
    }
}

ExprId FieldAccess(Checker& ck, const Node& node, ExprId base, const std::string& field) {
    TypeId type = ExprType(ck, base);
    if (type == kErrorType) {
        return base;
    }
    if (Kind(ck, type) != TypeKind::Struct) {
        return Fail(ck, node,
                    ShowType(ck, type) + " has no fields, so ." + field + " means nothing");
    }
    const StructDecl& sd = ck.out->structs[static_cast<std::size_t>(TypeOf(*ck.out, type).decl)];
    for (std::size_t i = 0; i < sd.fields.size(); i++) {
        if (sd.fields[i].name != field) {
            continue;
        }
        ExprId id = AddExpr(ck, ExprKind::Field, sd.fields[i].type, node);
        Expr& expr = ExprAt(ck, id);
        expr.a = base;
        expr.ref = static_cast<std::int32_t>(i);
        expr.value = sd.fields[i].offset;
        return id;
    }
    return Fail(ck, node, sd.name + " has no field " + field + "; its fields are " + FieldList(sd));
}

namespace {

// (at arr i x y): the element's field path
ExprId AtPath(Checker& ck, const Node& node, ExprId element) {
    ExprId value = element;
    for (std::size_t i = 3; i < node.items.size(); i++) {
        const Node& field = node.items[i];
        if (field.kind != NodeKind::Symbol || field.text.find('.') != std::string::npos) {
            return Fail(ck, field, "(at arr i field ...) takes plain field names, like x");
        }
        value = FieldAccess(ck, field, value, field.text);
        NoteField(ck, field, 0, value);
        if (ExprType(ck, value) == kErrorType) {
            return value;
        }
    }
    return value;
}

} // namespace

ExprId CheckAt(Checker& ck, const Node& node) {
    if (node.items.size() < 3) {
        return Fail(ck, node,
                    "at takes an array, an index and maybe field names; write (at arr i) or "
                    "(at arr i x)");
    }
    const Node& array_node = node.items[1];
    ExprId array = CheckExpr(ck, array_node, -1);
    TypeId type = ExprType(ck, array);
    if (type == kErrorType) {
        return ErrorExpr(ck, node);
    }
    if (Kind(ck, type) == TypeKind::Ptr) {
        std::string path;
        for (std::size_t i = 3; i < node.items.size(); i++) {
            path += " " + Show(node.items[i]);
        }
        return Fail(ck, node,
                    "at indexes arrays; through a pointer write (load (+ " + Show(array_node) +
                        " " + Show(node.items[2]) + ")" + path + ")");
    }
    if (Kind(ck, type) != TypeKind::Array) {
        return Fail(ck, array_node,
                    Show(array_node) + " is " + ShowType(ck, type) + ", not an array");
    }
    if (!IsPlaceLike(ExprAt(ck, array).kind)) {
        return Fail(ck, array_node, "at needs an array variable, field or element");
    }
    const Type& array_type = TypeOf(*ck.out, type);
    std::uint32_t count = array_type.count;
    TypeId elem = array_type.elem;
    TypeId index_type = count <= 256 ? kU8Type : kU16Type;
    ExprId index = CheckExpr(ck, node.items[2], index_type);
    std::string what =
        "the index into " + Show(array_node) + " (" + std::to_string(count) + " elements)";
    if (!RequireType(ck, node.items[2], index, index_type, what)) {
        return ErrorExpr(ck, node);
    }
    const Expr& index_expr = ExprAt(ck, index);
    // in an arm that never runs the index may be anything: (if (!= K 255) (set (at a K) 1))
    if (index_expr.kind == ExprKind::Literal && index_expr.type != kErrorType &&
        index_expr.value >= count && ck.at.dead == 0) {
        return Fail(ck, node.items[2],
                    "index " + std::to_string(index_expr.value) + " is past the end of " +
                        Show(array_node) + " (" + std::to_string(count) + " elements)");
    }
    ExprId id = AddExpr(ck, ExprKind::Index, elem, node);
    ExprAt(ck, id).a = array;
    ExprAt(ck, id).b = index;
    return AtPath(ck, node, id);
}

ExprId CheckDot(Checker& ck, const Node& node) {
    if (node.items.size() < 3) {
        return Fail(ck, node, "(. expr field ...) needs a value and field names");
    }
    std::string fields;
    for (std::size_t i = 2; i < node.items.size(); i++) {
        const Node& field = node.items[i];
        if (field.kind != NodeKind::Symbol || field.text.find('.') != std::string::npos) {
            return Fail(ck, field, "(. expr field ...) takes plain field names, like x");
        }
        fields += "." + field.text;
    }
    const Node& base_node = node.items[1];
    // a template cannot glue ,s and .x into one symbol, so only expanded code writes (. s x)
    if (base_node.kind == NodeKind::Symbol && node.expansion == 0) {
        return Fail(ck, node,
                    Show(node) + " is for macro templates; write " + base_node.text + fields);
    }
    const std::string& base_head = HeadName(base_node);
    if (base_head == "load" || (base_head == "at" && node.expansion == 0)) {
        std::string fix = "(" + base_head;
        for (std::size_t i = 1; i < base_node.items.size(); i++) {
            fix += " " + Show(base_node.items[i]);
        }
        for (std::size_t i = 2; i < node.items.size(); i++) {
            fix += " " + node.items[i].text;
        }
        return Fail(ck, node, Show(node) + " is for macro templates; write " + fix + ")");
    }
    if (node.expansion == 0) {
        return Fail(ck, node,
                    Show(node) +
                        " is for macro templates; write name.x, (at arr i x) or (load p x)");
    }
    ExprId base = CheckExpr(ck, base_node, -1);
    TypeId type = ExprType(ck, base);
    if (type == kErrorType) {
        return base;
    }
    if (Kind(ck, type) == TypeKind::Struct && !IsPlaceLike(ExprAt(ck, base).kind)) {
        return Fail(ck, base_node,
                    "take fields of a variable, (at ...) or a field, not " + Show(base_node));
    }
    ExprId value = base;
    for (std::size_t i = 2; i < node.items.size(); i++) {
        value = FieldAccess(ck, node, value, node.items[i].text);
        if (ExprType(ck, value) == kErrorType) {
            return value;
        }
    }
    return value;
}

namespace {

TypeId IoType(Checker& ck, const Node& node, const char* form) {
    if (node.kind == NodeKind::Symbol && node.text == "u8") {
        return kU8Type;
    }
    if (node.kind == NodeKind::Symbol && node.text == "u16") {
        return kU16Type;
    }
    Err(ck, node,
        std::string(form) + " moves u8 or u16 (low byte first); write (" + form + " u8 addr" +
            (std::string(form) == "io_write" ? " v)" : ")"));
    return kErrorType;
}

} // namespace

ExprId CheckIoRead(Checker& ck, const Node& node) {
    if (node.items.size() != 3) {
        return Fail(ck, node, "io_read takes a width and an address; write (io_read u8 a2.KBD)");
    }
    TypeId type = IoType(ck, node.items[1], "io_read");
    ExprId address = CheckExpr(ck, node.items[2], kU16Type);
    bool ok = RequireType(ck, node.items[2], address, kU16Type, "the io_read address");
    if (type == kErrorType || !ok) {
        return ErrorExpr(ck, node);
    }
    ExprId id = AddExpr(ck, ExprKind::IoRead, type, node);
    ExprAt(ck, id).a = address;
    return id;
}

ExprId CheckIoWrite(Checker& ck, const Node& node) {
    if (node.items.size() != 4) {
        return Fail(ck, node,
                    "io_write takes a width, an address and a value; write (io_write u8 addr v)");
    }
    TypeId type = IoType(ck, node.items[1], "io_write");
    ExprId address = CheckExpr(ck, node.items[2], kU16Type);
    bool ok = RequireType(ck, node.items[2], address, kU16Type, "the io_write address");
    ExprId value = CheckExpr(ck, node.items[3], type);
    ok = RequireType(ck, node.items[3], value, type, "the io_write value") && ok;
    if (type == kErrorType || !ok) {
        return ErrorExpr(ck, node);
    }
    ExprId id = AddExpr(ck, ExprKind::IoWrite, kVoidType, node);
    ExprAt(ck, id).a = address;
    ExprAt(ck, id).b = value;
    ExprAt(ck, id).value = TypeOf(*ck.out, type).size;
    return id;
}

ExprId CheckWaitCycles(Checker& ck, const Node& node) {
    if (node.items.size() != 2) {
        return Fail(ck, node, "wait_cycles takes one count of cycles; write (wait_cycles 100)");
    }
    const Node& count = node.items[1];
    ExprId n = CheckExpr(ck, count, kU16Type);
    if (!RequireType(ck, count, n, kU16Type, "the wait_cycles count")) {
        return ErrorExpr(ck, node);
    }
    if (ExprType(ck, n) == kErrorType) {
        return ErrorExpr(ck, node);
    }
    ExprId id = AddExpr(ck, ExprKind::WaitCycles, kVoidType, node);
    ExprAt(ck, id).a = n;
    std::uint16_t cycles = 0;
    std::size_t before = ck.errors->size();
    if (!FoldExpr(ck, n, cycles)) {
        return ck.errors->size() == before ? id : ErrorExpr(ck, node);
    }
    if (cycles == 0) {
        return Fail(ck, node, Show(node) + " waits for nothing; remove it");
    }
    if (cycles == 1) {
        return Fail(ck, node,
                    Show(node) + ": no 6502 instruction takes 1 cycle, so a wait is 2 or more; "
                                 "write (wait_cycles 2) or leave it out");
    }
    ExprAt(ck, id).value = cycles;
    return id;
}

namespace {

const char* const kJsrShape = "write (jsr addr (a v) (x v) (y v)), each register at most once, in "
                              "that order";

// 0, 1, 2 for a, x, y; -1 for anything else
int JsrRegister(const Node& pair) {
    if (pair.kind != NodeKind::List || pair.items.size() != 2 ||
        pair.items[0].kind != NodeKind::Symbol) {
        return -1;
    }
    const std::string& name = pair.items[0].text;
    if (name == "a") {
        return 0;
    }
    if (name == "x") {
        return 1;
    }
    return name == "y" ? 2 : -1;
}

} // namespace

ExprId CheckJsr(Checker& ck, const Node& node) {
    if (node.items.size() < 2 || node.items.size() > 5) {
        return Fail(ck, node,
                    std::string("jsr takes an address and up to three registers; ") + kJsrShape);
    }
    ExprId address = CheckExpr(ck, node.items[1], kU16Type);
    bool ok = RequireType(ck, node.items[1], address, kU16Type, "the jsr address");
    ExprId regs[3] = {-1, -1, -1};
    int last = -1;
    for (std::size_t i = 2; i < node.items.size(); i++) {
        const Node& pair = node.items[i];
        int reg = JsrRegister(pair);
        if (reg < 0) {
            return Fail(ck, pair, Show(pair) + " is not a register; " + kJsrShape);
        }
        if (reg <= last) {
            return Fail(ck, pair, Show(pair) + " is out of order or repeated; " + kJsrShape);
        }
        last = reg;
        static const char* const kNames[] = {"the jsr register a", "the jsr register x",
                                             "the jsr register y"};
        regs[reg] = CheckExpr(ck, pair.items[1], kU8Type);
        ok = RequireType(ck, pair.items[1], regs[reg], kU8Type, kNames[reg]) && ok;
    }
    if (!ok) {
        return ErrorExpr(ck, node);
    }
    ExprId xy = -1;
    if (regs[1] >= 0 || regs[2] >= 0) {
        xy = AddExpr(ck, ExprKind::JsrRegs, kVoidType, node);
        ExprAt(ck, xy).a = regs[1];
        ExprAt(ck, xy).b = regs[2];
    }
    ExprId id = AddExpr(ck, ExprKind::Jsr, kU8Type, node);
    ExprAt(ck, id).a = address;
    ExprAt(ck, id).b = regs[0];
    ExprAt(ck, id).c = xy;
    return id;
}

ExprId CheckCall(Checker& ck, const Node& node, std::int32_t function) {
    EnsureSignature(ck, function);
    const Function& fn = ck.out->functions[static_cast<std::size_t>(function)];
    const std::string& head = node.items[0].text;
    if (fn.interrupt) {
        return Fail(ck, node,
                    head + " is the interrupt handler: an IRQ runs it, a call never does");
    }
    std::size_t given = node.items.size() - 1;
    if (given != fn.params.size()) {
        std::string params = ParamText(ck, fn);
        return Fail(ck, node,
                    head + " takes " + std::to_string(fn.params.size()) + " argument" +
                        (fn.params.size() == 1 ? "" : "s") + ", got " + std::to_string(given) +
                        "; write (" + head + (params.empty() ? "" : " " + params) + ")");
    }
    std::vector<ExprId> args;
    bool ok = true;
    for (std::size_t i = 0; i < given; i++) {
        const Var& param = ck.out->vars[static_cast<std::size_t>(fn.params[i])];
        TypeId type = param.type;
        std::string param_name = param.name;
        ExprId arg = CheckExpr(ck, node.items[i + 1], type);
        ok = RequireType(ck, node.items[i + 1], arg, type,
                         "argument " + param_name + " of " + head) &&
             ok;
        args.push_back(arg);
    }
    AddCallee(ck, function);
    TypeId ret = ck.out->functions[static_cast<std::size_t>(function)].ret;
    ExprId id = AddExpr(ck, ExprKind::Call, ok ? ret : kErrorType, node);
    Expr& expr = ExprAt(ck, id);
    expr.ref = function;
    expr.first = AddList(ck, args);
    expr.count = static_cast<std::uint32_t>(args.size());
    return id;
}

} // namespace tisple
