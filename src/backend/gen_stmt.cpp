#include "backend/gen.hpp"

namespace tisple {

namespace {

const Expr& At(const Gen& g, ExprId id) {
    return ExprOf(*g.program, id);
}

bool IsStructType(const Gen& g, TypeId type) {
    return KindOf(*g.program, type) == TypeKind::Struct;
}

// (set x (+ x 1)) and (set x (- x 1)) as INC and DEC
bool IsStep(const Gen& g, ExprId place, ExprId value, bool& up) {
    const Expr& v = At(g, value);
    if (v.kind != ExprKind::Binary || (v.op != Op::Add && v.op != Op::Sub)) {
        return false;
    }
    if (HasEffects(*g.program, place)) {
        return false;
    }
    std::uint16_t c = 0;
    up = v.op == Op::Add;
    if (SameExpr(*g.program, v.a, place) && ConstValue(*g.program, v.b, c)) {
        if (c == 1) {
            return true;
        }
        std::uint16_t minus_one = ExprSize(*g.program, place) == 1 ? 0xFF : 0xFFFF;
        if (c == minus_one) {
            up = !up;
            return true;
        }
        return false;
    }
    return up && SameExpr(*g.program, v.b, place) && ConstValue(*g.program, v.a, c) && c == 1;
}

// (set place (+ place s)) or (+ s place) for a local s that is only ever 1 or -1: s, else -1
ExprId UnitStepOf(const Gen& g, ExprId place, ExprId value) {
    const Expr& v = At(g, value);
    if (v.kind != ExprKind::Binary || v.op != Op::Add || HasEffects(*g.program, place)) {
        return -1;
    }
    for (int side = 0; side < 2; side++) {
        ExprId self = side == 0 ? v.a : v.b;
        ExprId step = side == 0 ? v.b : v.a;
        const Expr& s = At(g, step);
        if (s.kind == ExprKind::VarRef && g.be->unit_steps[static_cast<std::size_t>(s.ref)] &&
            SameExpr(*g.program, self, place)) {
            return step;
        }
    }
    return -1;
}

bool GenStep(Gen& g, const Opnd& d, bool up) {
    if (!CanRmw(g, d, 0) || (d.size == 2 && !CanRmw(g, d, 1))) {
        return false;
    }
    if (d.size == 1) {
        RmwByte(g, up ? Mn::INC : Mn::DEC, d, 0);
        return true;
    }
    std::int32_t skip = NewLabel(g);
    if (up) {
        RmwByte(g, Mn::INC, d, 0);
        Branch(g, Mn::BNE, skip);
        RmwByte(g, Mn::INC, d, 1);
        NoteFollowStep(g, d, true, skip);
        PlaceLabel(g, skip);
        return true;
    }
    LoadA(g, d, 0);
    Branch(g, Mn::BNE, skip);
    RmwByte(g, Mn::DEC, d, 1);
    NoteFollowStep(g, d, false, skip);
    PlaceLabel(g, skip);
    RmwByte(g, Mn::DEC, d, 0);
    return true;
}

void GenLet(Gen& g, const Expr& e) {
    const IndexAlias& alias = g.be->aliases[static_cast<std::size_t>(e.ref)];
    if (alias.index_only || alias.forward) {
        return; // every read reads the value itself (index_alias.hpp)
    }
    for (std::int32_t var : g.reduced) {
        if (var == e.ref) {
            return; // set before the loop and moved at the end of each iteration
        }
    }
    const Var& var = g.program->vars[static_cast<std::size_t>(e.ref)];

    std::uint32_t size = TypeOf(*g.program, var.type).size;
    AsmValue base = SymValue(g.be->var_sym[static_cast<std::size_t>(e.ref)]);
    if (IsStructType(g, var.type)) {
        if (e.a < 0) {
            Emit(g, Mn::LDA, Mode::Imm, Num(0));
            for (std::uint32_t k = 0; k < size; k++) {
                Emit(g, Mn::STA, Mode::Abs, SymValue(base.sym, static_cast<std::int32_t>(k)));
            }
            return;
        }
        PlaceRef place;
        place.base = base;
        place.root = e.ref;
        place.cursor = VarCursor(g.be->shapes, e.ref);
        place.cursor.address = 0;
        place.type = var.type;
        CopyStruct(g, e.a, place, var.type);
        return;
    }
    Opnd dst = MemOpnd(base, size, 1);
    if (e.a < 0) {
        CopyOpnd(g, ImmOpnd(0, size), dst);
        return;
    }
    EvalInto(g, e.a, dst);
}

void GenIf(Gen& g, const Expr& e) {
    std::uint16_t c = 0;
    if (ConstValue(*g.program, e.a, c)) {
        if (c != 0) {
            GenStmt(g, e.b);
        } else if (e.c >= 0) {
            GenStmt(g, e.c);
        }
        return;
    }
    if (GenShiftedIf(g, e)) {
        return;
    }
    std::int32_t other = NewLabel(g);
    GenBranch(g, e.a, false, other);
    // each arm knows how the condition came out (gen_facts.cpp)
    std::size_t facts = PushFacts(g, e.a, true, -1);
    GenStmt(g, e.b);
    PopFacts(g, facts);
    if (e.c < 0) {
        PlaceLabel(g, other);
        return;
    }
    std::int32_t end = NewLabel(g);
    if (At(g, e.b).type != kNeverType) {
        Jump(g, end);
    }
    PlaceLabel(g, other);
    facts = PushFacts(g, e.a, false, -1);
    GenStmt(g, e.c);
    PopFacts(g, facts);
    PlaceLabel(g, end);
}

void GenReturn(Gen& g, const Expr& e) {
    if (!g.inlines.empty()) {
        InlineCtx ctx = g.inlines.back(); // a copy: the value may inline more calls
        if (e.a >= 0 && ctx.dst.size == 2) {
            EvalInto(g, e.a, ctx.dst);
        } else if (e.a >= 0) {
            EvalA(g, e.a);
        }
        Jump(g, ctx.ret_label);
        return;
    }
    if (e.a >= 0) {
        std::uint32_t size = ExprSize(*g.program, e.a);
        if (size == 2) {
            LoadAX(g, Eval(g, e.a, nullptr));
        } else {
            EvalA(g, e.a);
        }
    }
    Emit(g, Mn::RTS, Mode::Imp);
}

} // namespace

bool IsStepByOne(const Gen& g, ExprId place, ExprId value) {
    bool up = false;
    return IsStep(g, place, value, up) || UnitStepOf(g, place, value) >= 0;
}

void GenSet(Gen& g, ExprId place, ExprId value) {
    TypeId type = At(g, place).type;
    PlaceRef p;
    if (IsStructType(g, type)) {
        if (!ResolvePlace(g, place, value, p)) {
            return;
        }
        NormalizePlace(g, p, type);
        CopyStruct(g, value, p, type);
        return;
    }
    if (SetPinnedVar(g, place, value)) {
        return;
    }
    bool up = false;
    bool step = IsStep(g, place, value, up);
    ExprId unit = step ? -1 : UnitStepOf(g, place, value);
    if (!ResolvePlace(g, place, step || unit >= 0 ? -1 : value, p)) {
        return;
    }
    NormalizePlace(g, p, type);
    PinPlace(g, p, type);
    Opnd d = PlaceOpnd(g, p, type);
    Opnd s = unit >= 0 ? TrySimple(g, unit) : Opnd();
    bool rmw = CanRmw(g, d, 0) && (d.size == 1 || CanRmw(g, d, 1));
    if (unit >= 0 && rmw && s.size == d.size) {
        // a step of 1 or -1: its sign picks INC or DEC
        std::int32_t down = NewLabel(g);
        std::int32_t done = NewLabel(g);
        LoadA(g, s, static_cast<int>(s.size) - 1);
        Branch(g, Mn::BMI, down);
        GenStep(g, d, true);
        Jump(g, done);
        PlaceLabel(g, down);
        GenStep(g, d, false);
        PlaceLabel(g, done);
    } else if (!step || !GenStep(g, d, up)) {
        EvalInto(g, value, d);
        if (!g.follows.empty()) {
            std::int32_t after = NewLabel(g);
            NoteFollowSet(g, d, after);
            PlaceLabel(g, after);
        }
    }
    NoteSet(g, place, value);
}

void GenStore(Gen& g, const Expr& e) {
    PlaceRef p;
    if (!ResolveStore(g, e, e.b, p)) {
        return;
    }
    NormalizePlace(g, p, p.type);
    PinPlace(g, p, p.type);
    if (IsStructType(g, p.type)) {
        CopyStruct(g, e.b, p, p.type);
        return;
    }
    EvalInto(g, e.b, PlaceOpnd(g, p, p.type));
}

void GenStmt(Gen& g, ExprId id) {
    const Expr& e = At(g, id);
    if (e.kind != ExprKind::Do) {
        SourceNote(g, e.loc);
        // a do's forms each forget what they may change, in turn
        KillFacts(g, id);
    }
    TempMark mark = MarkTemps(g);
    switch (e.kind) {
    case ExprKind::Do:
        for (std::uint32_t i = 0; i < e.count; i++) {
            ExprId item = ListItem(*g.program, e, i);
            GenStmt(g, item);
            if (At(g, item).type == kNeverType) {
                break;
            }
        }
        break;
    case ExprKind::Let:
        GenLet(g, e);
        break;
    case ExprKind::Set:
        GenSet(g, e.a, e.b);
        break;
    case ExprKind::Store:
    case ExprKind::StoreField:
        GenStore(g, e);
        break;
    case ExprKind::If:
        if (e.type == kVoidType || e.type == kNeverType) {
            GenIf(g, e);
        } else {
            Eval(g, id, nullptr);
        }
        break;
    case ExprKind::While:
        GenWhile(g, id);
        break;
    case ExprKind::For:
        GenFor(g, id);
        break;
    case ExprKind::Match:
        GenMatch(g, e, nullptr, false);
        break;
    case ExprKind::Return:
        GenReturn(g, e);
        break;
    case ExprKind::Break:
        Jump(g, g.loops.back().break_label);
        break;
    case ExprKind::Continue:
        Jump(g, g.loops.back().continue_label);
        break;
    case ExprKind::IoRead:
    case ExprKind::IoWrite:
        GenIo(g, id);
        break;
    case ExprKind::Call:
        EvalCall(g, id, nullptr);
        break;
    case ExprKind::Jsr:
        EvalJsr(g, id);
        break;
    case ExprKind::WaitCycles:
        GenWait(g, id);
        break;
    default:
        // a value nobody uses: only its effects matter
        if (HasEffects(*g.program, id)) {
            Eval(g, id, nullptr);
        }
        break;
    }
    ForgetDivision(g, id);
    ReleaseTemps(g, mark);
}

void GenFunction(Backend& be, std::int32_t function) {
    const Program& program = *be.program;
    const Function& fn = program.functions[static_cast<std::size_t>(function)];
    Gen g;
    g.be = &be;
    g.program = &program;
    g.function = function;
    g.out = &be.code[static_cast<std::size_t>(function)];
    PlaceLabel(g, be.fn_sym[static_cast<std::size_t>(function)]);
    std::uint32_t size = TypeOf(program, fn.ret).size;
    if (fn.ret == kVoidType || size == 0) {
        GenStmt(g, fn.body);
    } else if (size == 2) {
        LoadAX(g, Eval(g, fn.body, nullptr));
    } else {
        EvalA(g, fn.body);
    }
    if (ExprOf(program, fn.body).type != kNeverType) {
        Emit(g, Mn::RTS, Mode::Imp);
    }
    InitPagePointers(g);
}

} // namespace tisple
