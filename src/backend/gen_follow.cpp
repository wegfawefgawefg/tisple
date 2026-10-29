#include "backend/gen.hpp"

#include <algorithm>

// Page pointers (gen_index.cpp) whose index is a u16 variable a while loop only steps by one
// (up, down, or by a local that is only ever 1 or -1: Bresenham's x) keep their high byte up
// to date across the loop instead of working it out at every use: it is set once before the
// loop, and moved by INC or DEC where a step carries into the variable's high byte. line's
// plot reads COL_BYTE and COL_BIT at x for every pixel; their pointers' high bytes now change
// only when x crosses a page.
//
// The steps are generated before all the pointers are known, so each step notes where its
// high byte moves (the label after it), and the code for the pointers goes in once the loop is
// done (EndFollow), like the wide loop's fixups (gen_while_wide.cpp).

namespace tisple {

namespace {

constexpr int kMaxDepth = 64;

const Expr& At(const Gen& g, ExprId id) {
    return ExprOf(*g.program, id);
}

// the loop has loops inside (whose register choices could take the variable) or sets var some
// other way than a step by one
void ScanSets(const Gen& g, ExprId id, int depth, std::vector<std::uint8_t>& state, bool& nested) {
    if (id < 0 || depth > kMaxDepth) {
        return;
    }
    const Program& program = *g.program;
    const Expr& e = ExprOf(program, id);
    if (depth > 0 && (e.kind == ExprKind::While || e.kind == ExprKind::For)) {
        nested = true;
        return;
    }
    if (e.kind == ExprKind::Set && At(g, e.a).kind == ExprKind::VarRef) {
        std::size_t var = static_cast<std::size_t>(At(g, e.a).ref);
        // 1: only steps by one so far, 2: something else
        state[var] = IsStepByOne(g, e.a, e.b) && state[var] != 2 ? 1 : 2;
    }
    // a let inside the loop starts the variable afresh on every pass: not a step
    if (e.kind == ExprKind::Let && e.ref >= 0) {
        state[static_cast<std::size_t>(e.ref)] = 2;
    }
    ScanSets(g, e.a, depth + 1, state, nested);
    ScanSets(g, e.b, depth + 1, state, nested);
    ScanSets(g, e.c, depth + 1, state, nested);
    if (e.kind == ExprKind::Do || e.kind == ExprKind::Call || e.kind == ExprKind::StructLit) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            ScanSets(g, ListItem(program, e, i), depth + 1, state, nested);
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            ScanSets(g, program.clauses[e.first + i].body, depth + 1, state, nested);
        }
    }
}

AsmValue VarByte(const Gen& g, std::int32_t var, int k) {
    AsmValue v = SymValue(g.be->var_sym[static_cast<std::size_t>(var)]);
    v.offset += k;
    return v;
}

bool SameByte(const AsmValue& a, const AsmValue& b) {
    return a.sym == b.sym && a.offset == b.offset && a.part == b.part;
}

// the follow for the variable whose two bytes o is, or nullptr
Follow* FindFollow(Gen& g, const Opnd& o) {
    if (o.size != 2 || o.in_a || o.b[0].kind != ByteKind::Mem || o.b[1].kind != ByteKind::Mem) {
        return nullptr;
    }
    for (Follow& f : g.follows) {
        if (SameByte(o.b[0].value, VarByte(g, f.var, 0)) &&
            SameByte(o.b[1].value, VarByte(g, f.var, 1))) {
            return &f;
        }
    }
    return nullptr;
}

std::size_t LabelAt(const Gen& g, std::size_t first, std::int32_t label) {
    for (std::size_t i = first; i < g.out->size(); i++) {
        const AsmItem& item = (*g.out)[i];
        if (item.kind == ItemKind::Label && item.value.sym == label) {
            return i;
        }
    }
    return g.out->size();
}

} // namespace

bool BeginFollow(Gen& g, ExprId loop) {
    if (!g.follows.empty()) {
        return false; // an outer loop's
    }
    const Program& program = *g.program;
    std::vector<std::uint8_t> state(program.vars.size(), 0);
    bool nested = false;
    ScanSets(g, loop, 0, state, nested);
    if (nested) {
        return false;
    }
    for (std::size_t v = 0; v < state.size(); v++) {
        const Var& var = program.vars[v];
        std::int32_t id = static_cast<std::int32_t>(v);
        if (state[v] != 1 || TypeOf(program, var.type).size != 2 || var.address_taken ||
            var.loop_var || (var.kind != VarKind::Local && var.kind != VarKind::Param) ||
            g.pin[0] == id || g.pin[1] == id || (g.wide && g.wide->var == id) ||
            IsReplacedCounter(g, id) || g.be->aliases[v].value >= 0) {
            continue;
        }
        Follow f;
        f.var = id;
        g.follows.push_back(f);
    }
    return !g.follows.empty();
}

bool FollowPagePointer(Gen& g, const Opnd& index, std::size_t ptr) {
    Follow* f = FindFollow(g, index);
    if (!f) {
        return false;
    }
    if (std::find(f->ptrs.begin(), f->ptrs.end(), ptr) == f->ptrs.end()) {
        f->ptrs.push_back(ptr);
    }
    return true;
}

void NoteFollowStep(Gen& g, const Opnd& d, bool up, std::int32_t label) {
    Follow* f = FindFollow(g, d);
    if (f) {
        f->steps.push_back({label, up});
    }
}

void NoteFollowSet(Gen& g, const Opnd& d, std::int32_t label) {
    Follow* f = FindFollow(g, d);
    if (f) {
        f->sets.push_back(label);
    }
}

namespace {

// the pointers' high bytes from the variable's
void SetHighBytes(Gen& g, const Follow& f) {
    for (std::size_t p : f.ptrs) {
        const PagePointer& pp = g.page_ptrs[p];
        AsmValue high = pp.ptr;
        high.offset += 1;
        Emit(g, Mn::LDA, Mode::Abs, VarByte(g, f.var, 1));
        Emit(g, Mn::CLC, Mode::Imp);
        OpA(g, Mn::ADC, SymImmOpnd(pp.base, 2), 1);
        Emit(g, Mn::STA, Mode::Abs, high);
    }
}

} // namespace

void EndFollow(Gen& g, std::size_t init_at) {
    std::vector<Follow> follows = g.follows;
    g.follows.clear();
    for (const Follow& f : follows) {
        if (f.ptrs.empty()) {
            continue;
        }
        // a set that is not a step: the high bytes again
        for (std::int32_t label : f.sets) {
            std::size_t at = LabelAt(g, init_at, label);
            if (at == g.out->size()) {
                continue;
            }
            std::size_t before = g.out->size();
            SetHighBytes(g, f);
            std::rotate(g.out->begin() + static_cast<std::ptrdiff_t>(at),
                        g.out->begin() + static_cast<std::ptrdiff_t>(before), g.out->end());
        }
        for (const auto& step : f.steps) {
            std::size_t at = LabelAt(g, init_at, step.first);
            if (at == g.out->size()) {
                continue;
            }
            std::size_t before = g.out->size();
            for (std::size_t p : f.ptrs) {
                AsmValue high = g.page_ptrs[p].ptr;
                high.offset += 1;
                Emit(g, step.second ? Mn::INC : Mn::DEC, Mode::Abs, high);
            }
            std::rotate(g.out->begin() + static_cast<std::ptrdiff_t>(at),
                        g.out->begin() + static_cast<std::ptrdiff_t>(before), g.out->end());
        }
        // the high bytes for the variable as the loop starts, once each time it does
        g.loop_depth--;
        std::size_t before = g.out->size();
        SetHighBytes(g, f);
        g.loop_depth++;
        std::rotate(g.out->begin() + static_cast<std::ptrdiff_t>(init_at),
                    g.out->begin() + static_cast<std::ptrdiff_t>(before), g.out->end());
    }
}

} // namespace tisple
