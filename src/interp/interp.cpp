#include "interp/interp.hpp"

#include "check/arith.hpp"

namespace tisple {

namespace {

const Expr& At(const Interp& in, ExprId id) {
    return in.program->exprs[static_cast<std::size_t>(id)];
}

const Type& TypeAt(const Interp& in, TypeId id) {
    return in.program->types[static_cast<std::size_t>(id)];
}

bool IsScalar(const Interp& in, TypeId type) {
    return IsScalarKind(TypeAt(in, type).kind);
}

void Stop(Interp& in, StopKind kind, const std::string& message, const Loc& loc) {
    if (in.flow == Flow::Stop) {
        return;
    }
    in.flow = Flow::Stop;
    in.stop = kind;
    in.message = message;
    in.stop_loc = loc;
}

std::uint16_t Eval(Interp& in, ExprId id);

// ---- values as packed bytes, moved through cursors field by field

void ReadValue(const Interp& in, Cursor src, TypeId type, std::vector<std::uint8_t>& bytes,
               std::uint32_t offset) {
    const Type& t = TypeAt(in, type);
    if (IsScalarKind(t.kind)) {
        std::uint16_t value = ReadScalar(in, src, t.size);
        bytes[offset] = static_cast<std::uint8_t>(value & 0xFF);
        if (t.size == 2) {
            bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        }
        return;
    }
    if (t.kind == TypeKind::Struct) {
        for (const Field& field : in.program->structs[static_cast<std::size_t>(t.decl)].fields) {
            ReadValue(in, FieldCursor(src, field.offset), field.type, bytes, offset + field.offset);
        }
        return;
    }
    std::uint32_t elem_size = TypeAt(in, t.elem).size;
    for (std::uint32_t i = 0; i < t.count; i++) {
        Cursor element = ElementCursor(*in.program, *in.layout, -1, src, type, i);
        ReadValue(in, element, t.elem, bytes, offset + i * elem_size);
    }
}

void WriteValue(Interp& in, Cursor dst, std::int32_t root, TypeId type,
                const std::vector<std::uint8_t>& bytes, std::uint32_t offset) {
    const Type& t = TypeAt(in, type);
    if (IsScalarKind(t.kind)) {
        std::uint16_t value = bytes[offset];
        if (t.size == 2) {
            value = static_cast<std::uint16_t>(value | (bytes[offset + 1] << 8));
        }
        WriteScalar(in, dst, t.size, value);
        return;
    }
    if (t.kind == TypeKind::Struct) {
        for (const Field& field : in.program->structs[static_cast<std::size_t>(t.decl)].fields) {
            WriteValue(in, FieldCursor(dst, field.offset), root, field.type, bytes,
                       offset + field.offset);
        }
        return;
    }
    std::uint32_t elem_size = TypeAt(in, t.elem).size;
    for (std::uint32_t i = 0; i < t.count; i++) {
        Cursor element = ElementCursor(*in.program, *in.layout, root, dst, type, i);
        WriteValue(in, element, root, t.elem, bytes, offset + i * elem_size);
    }
}

// a place, or the target of a Load: where its bytes are
Cursor EvalPlace(Interp& in, ExprId id, std::int32_t& root) {
    const Expr& e = At(in, id);
    switch (e.kind) {
    case ExprKind::VarRef:
        root = e.ref;
        return VarCursor(*in.layout, e.ref);
    case ExprKind::Field:
        return FieldCursor(EvalPlace(in, e.a, root), e.value);
    case ExprKind::Index: {
        Cursor base = EvalPlace(in, e.a, root);
        std::uint16_t index = Eval(in, e.b);
        return ElementCursor(*in.program, *in.layout, root, base, At(in, e.a).type, index);
    }
    case ExprKind::Load:
    case ExprKind::LoadField: {
        root = -1;
        Cursor cursor;
        cursor.address = static_cast<std::uint16_t>((Eval(in, e.a) + e.value) & 0xFFFF);
        return cursor;
    }
    default:
        root = -1;
        Stop(in, StopKind::Error, "internal: not a place", e.loc);
        return Cursor();
    }
}

// the whole value of a struct-typed expression, in packed bytes
void EvalValue(Interp& in, ExprId id, std::vector<std::uint8_t>& bytes, std::uint32_t offset) {
    const Expr& e = At(in, id);
    const Type& t = TypeAt(in, e.type);
    if (IsScalarKind(t.kind)) {
        std::uint16_t value = Eval(in, id);
        bytes[offset] = static_cast<std::uint8_t>(value & 0xFF);
        if (t.size == 2) {
            bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        }
        return;
    }
    if (e.kind == ExprKind::StructLit) {
        const StructDecl& sd = in.program->structs[static_cast<std::size_t>(t.decl)];
        for (std::uint32_t i = 0; i < e.count; i++) {
            ExprId field = in.program->lists[e.first + i];
            EvalValue(in, field, bytes, offset + sd.fields[i].offset);
        }
        return;
    }
    std::int32_t root = -1;
    Cursor src = EvalPlace(in, id, root);
    ReadValue(in, src, e.type, bytes, offset);
}

void Assign(Interp& in, Cursor dst, std::int32_t root, TypeId type, ExprId value) {
    const Type& t = TypeAt(in, type);
    if (IsScalarKind(t.kind)) {
        std::uint16_t raw = Eval(in, value);
        if (in.flow != Flow::Stop) {
            WriteScalar(in, dst, t.size, raw);
        }
        return;
    }
    std::vector<std::uint8_t> bytes(t.size, 0);
    EvalValue(in, value, bytes, 0);
    if (in.flow != Flow::Stop) {
        WriteValue(in, dst, root, type, bytes, 0);
    }
}

std::uint16_t CallInto(Interp& in, std::int32_t function, const std::uint16_t* args,
                       std::uint32_t count) {
    const Function& fn = in.program->functions[static_cast<std::size_t>(function)];
    for (std::uint32_t i = 0; i < count; i++) {
        std::int32_t param = fn.params[i];
        TypeId type = in.program->vars[static_cast<std::size_t>(param)].type;
        WriteScalar(in, VarCursor(*in.layout, param), TypeAt(in, type).size, args[i]);
    }
    std::uint16_t value = Eval(in, fn.body);
    if (in.flow == Flow::Return) {
        in.flow = Flow::Normal;
        value = in.return_value;
    }
    return fn.ret == kVoidType ? 0 : value;
}

std::uint16_t EvalCall(Interp& in, const Expr& e) {
    std::uint16_t args[64];
    std::vector<std::uint16_t> many;
    std::uint16_t* slots = args;
    if (e.count > 64) {
        many.resize(e.count);
        slots = many.data();
    }
    for (std::uint32_t i = 0; i < e.count; i++) {
        slots[i] = Eval(in, in.program->lists[e.first + i]);
        if (in.flow == Flow::Stop) {
            return 0;
        }
    }
    return CallInto(in, e.ref, slots, e.count);
}

std::uint16_t EvalFor(Interp& in, const Expr& e) {
    std::uint16_t start = Eval(in, e.a);
    std::uint16_t end = e.b >= 0 ? Eval(in, e.b) : 0;
    if (in.flow != Flow::Normal) {
        return 0;
    }
    TypeId type = in.program->vars[static_cast<std::size_t>(e.ref)].type;
    TypeKind kind = TypeAt(in, type).kind;
    std::uint32_t size = TypeAt(in, type).size;
    // runs over the mathematical range [start, end); no end means one past the type's max
    std::int32_t first = SignedValue(kind, start);
    std::int32_t last = SignedValue(kind, end);
    if (e.b < 0) {
        last = (KindBits(kind) == 8 ? 256 : 65536) / (IsSignedKind(kind) ? 2 : 1);
    }
    Cursor var = VarCursor(*in.layout, e.ref);
    for (std::int32_t i = first; i < last; i++) {
        WriteScalar(in, var, size, MaskToKind(kind, static_cast<std::uint32_t>(i)));
        Eval(in, e.c);
        if (in.flow == Flow::Break) {
            in.flow = Flow::Normal;
            break;
        }
        if (in.flow == Flow::Continue) {
            in.flow = Flow::Normal;
        }
        if (in.flow != Flow::Normal) {
            break;
        }
    }
    return 0;
}

std::uint16_t EvalWhile(Interp& in, const Expr& e) {
    while (true) {
        std::uint16_t cond = Eval(in, e.a);
        if (in.flow != Flow::Normal || cond == 0) {
            return 0;
        }
        Eval(in, e.b);
        if (in.flow == Flow::Break) {
            in.flow = Flow::Normal;
            return 0;
        }
        if (in.flow == Flow::Continue) {
            in.flow = Flow::Normal;
        }
        if (in.flow != Flow::Normal) {
            return 0;
        }
    }
}

std::uint16_t EvalMatch(Interp& in, const Expr& e) {
    std::uint16_t value = Eval(in, e.a);
    if (in.flow != Flow::Normal) {
        return 0;
    }
    for (std::uint32_t i = 0; i < e.count; i++) {
        const Clause& clause = in.program->clauses[e.first + i];
        if (clause.wildcard || clause.value == value) {
            return Eval(in, clause.body);
        }
    }
    return 0;
}

std::uint16_t EvalDo(Interp& in, const Expr& e) {
    std::uint16_t value = 0;
    for (std::uint32_t i = 0; i < e.count; i++) {
        value = Eval(in, in.program->lists[e.first + i]);
        if (in.flow != Flow::Normal) {
            return 0;
        }
    }
    return value;
}

std::uint16_t EvalBinary(Interp& in, const Expr& e) {
    std::uint16_t a = Eval(in, e.a);
    std::uint16_t b = Eval(in, e.b);
    if (in.flow != Flow::Normal) {
        return 0;
    }
    // a division by zero has a defined result (check/arith.hpp), so it does not stop
    bool div_by_zero = false;
    return EvalBinaryOp(e.op, TypeAt(in, At(in, e.a).type).kind, a, b, div_by_zero);
}

std::uint16_t EvalIo(Interp& in, const Expr& e) {
    std::uint16_t address = Eval(in, e.a);
    if (e.kind == ExprKind::IoRead) {
        if (in.flow != Flow::Normal) {
            return 0;
        }
        std::uint16_t low = in.io.read ? in.io.read(address) : 0;
        if (TypeAt(in, e.type).size == 1) {
            return low;
        }
        std::uint16_t high = in.io.read ? in.io.read(static_cast<std::uint16_t>(address + 1)) : 0;
        return static_cast<std::uint16_t>((low & 0xFF) | (high << 8));
    }
    std::uint16_t value = Eval(in, e.b);
    if (in.flow != Flow::Normal || !in.io.write) {
        return 0;
    }
    in.io.write(address, static_cast<std::uint8_t>(value & 0xFF));
    if (e.value == 2) {
        in.io.write(static_cast<std::uint16_t>(address + 1), static_cast<std::uint8_t>(value >> 8));
    }
    return 0;
}

std::uint16_t Eval(Interp& in, ExprId id) {
    if (in.flow == Flow::Stop) {
        return 0;
    }
    const Expr& e = At(in, id);
    in.steps++;
    if (in.steps > in.max_steps) {
        Stop(in, StopKind::StepLimit, "step limit of " + std::to_string(in.max_steps) + " reached",
             e.loc);
        return 0;
    }
    switch (e.kind) {
    case ExprKind::Literal:
        return static_cast<std::uint16_t>(e.value);
    case ExprKind::VarRef:
    case ExprKind::Index:
    case ExprKind::Field:
    case ExprKind::Load:
    case ExprKind::LoadField: {
        std::int32_t root = -1;
        Cursor cursor = EvalPlace(in, id, root);
        if (in.flow != Flow::Normal || !IsScalar(in, e.type)) {
            return 0;
        }
        return ReadScalar(in, cursor, TypeAt(in, e.type).size);
    }
    case ExprKind::Store: {
        Cursor cursor;
        cursor.address = Eval(in, e.a);
        TypeId elem = TypeAt(in, At(in, e.a).type).elem;
        Assign(in, cursor, -1, elem, e.b);
        return 0;
    }
    case ExprKind::StoreField: {
        Cursor cursor;
        cursor.address = static_cast<std::uint16_t>((Eval(in, e.a) + e.value) & 0xFFFF);
        Assign(in, cursor, -1, e.ref, e.b);
        return 0;
    }
    case ExprKind::AddrOf: {
        std::int32_t root = -1;
        return EvalPlace(in, e.a, root).address;
    }
    case ExprKind::Unary: {
        std::uint16_t a = Eval(in, e.a);
        return EvalUnaryOp(e.op, TypeAt(in, e.type).kind, a);
    }
    case ExprKind::Binary:
        return EvalBinary(in, e);
    case ExprKind::And:
        return Eval(in, e.a) != 0 && in.flow == Flow::Normal ? Eval(in, e.b) : 0;
    case ExprKind::Or:
        return Eval(in, e.a) != 0 ? 1 : Eval(in, e.b);
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub: {
        std::uint32_t p = Eval(in, e.a);
        std::uint32_t bytes = static_cast<std::uint32_t>(Eval(in, e.b)) * e.value;
        return static_cast<std::uint16_t>((e.kind == ExprKind::PtrAdd ? p + bytes : p - bytes) &
                                          0xFFFF);
    }
    case ExprKind::Convert: {
        std::uint16_t a = Eval(in, e.a);
        return ConvertBits(TypeAt(in, At(in, e.a).type).kind, TypeAt(in, e.type).kind, a);
    }
    case ExprKind::Reinterpret:
        return Eval(in, e.a);
    case ExprKind::Call:
        return EvalCall(in, e);
    case ExprKind::IoRead:
    case ExprKind::IoWrite:
        return EvalIo(in, e);
    case ExprKind::JsrRegs: // only read through its Jsr
    case ExprKind::Jsr:
        // machine code runs only on the 6502: in the emulator, or on a real Apple II
        Stop(in, StopKind::Error,
             "jsr needs the machine: the interpreter cannot run 6502 code; use tisple run", e.loc);
        return 0;
    case ExprKind::StructLit:
        for (std::uint32_t i = 0; i < e.count && in.flow == Flow::Normal; i++) {
            Eval(in, in.program->lists[e.first + i]);
        }
        return 0;
    case ExprKind::If: {
        std::uint16_t cond = Eval(in, e.a);
        if (in.flow != Flow::Normal) {
            return 0;
        }
        if (cond != 0) {
            return Eval(in, e.b);
        }
        return e.c >= 0 ? Eval(in, e.c) : 0;
    }
    case ExprKind::Do:
        return EvalDo(in, e);
    case ExprKind::While:
        return EvalWhile(in, e);
    case ExprKind::For:
        return EvalFor(in, e);
    case ExprKind::Match:
        return EvalMatch(in, e);
    case ExprKind::Return: {
        std::uint16_t value = e.a >= 0 ? Eval(in, e.a) : 0;
        if (in.flow == Flow::Normal) {
            in.return_value = value;
            in.flow = Flow::Return;
        }
        return 0;
    }
    case ExprKind::Break:
        in.flow = Flow::Break;
        return 0;
    case ExprKind::Continue:
        in.flow = Flow::Continue;
        return 0;
    case ExprKind::Let: {
        Cursor var = VarCursor(*in.layout, e.ref);
        TypeId type = in.program->vars[static_cast<std::size_t>(e.ref)].type;
        Assign(in, var, e.ref, type, e.a);
        return 0;
    }
    case ExprKind::Set: {
        std::int32_t root = -1;
        Cursor place = EvalPlace(in, e.a, root);
        if (in.flow != Flow::Normal) {
            return 0;
        }
        Assign(in, place, root, At(in, e.a).type, e.b);
        return 0;
    }
    }
    return 0;
}

} // namespace

std::uint16_t ReadScalar(const Interp& in, Cursor cursor, std::uint32_t size) {
    std::uint16_t low = in.memory[cursor.address];
    if (size == 1) {
        return low;
    }
    std::uint16_t high = in.memory[CursorByte(cursor, 1)];
    return static_cast<std::uint16_t>(low | (high << 8));
}

void WriteScalar(Interp& in, Cursor cursor, std::uint32_t size, std::uint16_t value) {
    in.memory[cursor.address] = static_cast<std::uint8_t>(value & 0xFF);
    if (size == 2) {
        in.memory[CursorByte(cursor, 1)] = static_cast<std::uint8_t>(value >> 8);
    }
}

void InitInterp(Interp& in, const Program& program, const Layout& layout) {
    in.program = &program;
    in.layout = &layout;
    in.memory.assign(0x10000, 0);
    for (std::size_t i = 0; i < program.vars.size(); i++) {
        const Var& var = program.vars[i];
        if (var.init.empty() || var.type == kErrorType) {
            continue;
        }
        std::int32_t id = static_cast<std::int32_t>(i);
        WriteValue(in, VarCursor(layout, id), id, var.type, var.init, 0);
    }
}

InterpResult CallFunction(Interp& in, std::int32_t function,
                          const std::vector<std::uint16_t>& args) {
    in.steps = 0;
    in.flow = Flow::Normal;
    in.stop = StopKind::Finished;
    in.message.clear();
    InterpResult result;
    const Function& fn = in.program->functions[static_cast<std::size_t>(function)];
    if (args.size() != fn.params.size()) {
        result.kind = StopKind::Error;
        result.message = fn.name + " takes " + std::to_string(fn.params.size()) + " arguments";
        result.loc = fn.loc;
        return result;
    }
    std::uint16_t value =
        CallInto(in, function, args.data(), static_cast<std::uint32_t>(args.size()));
    result.steps = in.steps;
    if (in.flow == Flow::Stop) {
        result.kind = in.stop;
        result.message = in.message;
        result.loc = in.stop_loc;
        return result;
    }
    result.kind = StopKind::Finished;
    result.type = fn.ret;
    result.has_value = fn.ret != kVoidType;
    result.value = value;
    return result;
}

} // namespace tisple
