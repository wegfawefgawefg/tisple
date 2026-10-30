#include "backend/gen.hpp"

namespace tisple {

namespace {

struct Leaf {
    PlaceRef place;
    TypeId type = -1;
};

// every scalar inside a struct value, in packed order
void Leaves(const Gen& g, const PlaceRef& place, TypeId type, std::vector<Leaf>& out) {
    const Type& t = TypeOf(*g.program, type);
    if (IsScalarKind(t.kind)) {
        Leaf leaf;
        leaf.place = place;
        leaf.type = type;
        out.push_back(leaf);
        return;
    }
    if (t.kind == TypeKind::Struct) {
        for (const Field& f : g.program->structs[static_cast<std::size_t>(t.decl)].fields) {
            Leaves(g, FieldPlace(g, place, f.offset), f.type, out);
        }
        return;
    }
    for (std::uint32_t i = 0; i < t.count; i++) {
        Leaves(g, ElementPlace(g, place, type, i), t.elem, out);
    }
}

// the scalars of a struct at a place, in packed order, each readable later
void CollectPlace(Gen& g, const PlaceRef& source, TypeId type, bool snapshot,
                  std::vector<Opnd>& out) {
    std::vector<Leaf> leaves;
    Leaves(g, source, type, leaves);
    for (const Leaf& leaf : leaves) {
        Opnd o = PlaceOpnd(g, leaf.place, leaf.type);
        out.push_back(snapshot ? Snapshot(g, o) : o);
    }
}

// the value's scalars in packed order, each readable later
void CollectValue(Gen& g, ExprId id, TypeId type, bool snapshot, std::vector<Opnd>& out) {
    const Expr& e = ExprOf(*g.program, id);
    if (e.kind == ExprKind::StructLit) {
        const StructDecl& sd =
            g.program->structs[static_cast<std::size_t>(TypeOf(*g.program, type).decl)];
        for (std::uint32_t i = 0; i < e.count; i++) {
            ExprId field = ListItem(*g.program, e, i);
            TypeId field_type = sd.fields[i].type;
            if (!IsScalarKind(KindOf(*g.program, field_type))) {
                CollectValue(g, field, field_type, snapshot, out);
                continue;
            }
            Opnd o = Eval(g, field, nullptr);
            if (o.in_a || (snapshot && IsVolatile(o))) {
                o = Snapshot(g, o);
            }
            out.push_back(o);
        }
        return;
    }
    PlaceRef source;
    if (!ResolvePlace(g, id, -1, source)) {
        return;
    }
    NormalizePlace(g, source, type);
    CollectPlace(g, source, type, snapshot, out);
}

// a struct copy this big or bigger that would read its value before it writes (through
// pointers, which may name the same place) goes by a loop: 16 cycles a byte through two
// pointers, against 21 for each byte read into zero page and stored from there, and a few
// bytes of code instead of ten a byte
constexpr std::uint32_t kMinLoopCopy = 6;
constexpr std::uint32_t kMaxLoopCopy = 128; // Y counts down to 0 with BPL

// every byte at base + 0 to size - 1 with Y: a static place, or a pointer with no offset
bool LoopPlace(Gen& g, PlaceRef& place) {
    if (place.has_index || place.cursor.split || place.cursor.stride != 1 || place.delta != 0) {
        return false;
    }
    if (place.pointer && place.ptr2.sym >= 0) {
        return false;
    }
    if (place.pointer && place.cursor.address != 0) {
        PlaceToPointer(g, place);
    }
    return true;
}

AsmValue LoopAddress(const PlaceRef& place) {
    if (place.pointer) {
        return place.ptr;
    }
    AsmValue address = place.base;
    address.offset = static_cast<std::int32_t>((address.offset + place.cursor.address) & 0xFFFF);
    return address;
}

// the copy as a loop, down from the last byte. Two places of one struct type either are the
// same or do not overlap: a pointer reaches only inside the variable it was made from
// (docs/language.md, Pointer provenance), and a struct holds no struct of its own type
void LoopCopy(Gen& g, const PlaceRef& src, const PlaceRef& dst, std::uint32_t size) {
    g.loop_factors.resize(static_cast<std::size_t>(g.loop_depth));
    g.loop_factors.push_back(std::min<std::uint32_t>(size, kMaxWeighedTrips));
    g.loop_runs.resize(static_cast<std::size_t>(g.loop_depth));
    g.loop_runs.push_back(size);
    Emit(g, Mn::LDY, Mode::Imm, Num(static_cast<std::uint16_t>(size - 1)));
    std::int32_t top = NewLabel(g);
    PlaceLabel(g, top);
    g.loop_depth++;
    Emit(g, Mn::LDA, src.pointer ? Mode::IndY : Mode::AbsY, LoopAddress(src));
    Emit(g, Mn::STA, dst.pointer ? Mode::IndY : Mode::AbsY, LoopAddress(dst));
    Emit(g, Mn::DEY, Mode::Imp);
    Branch(g, Mn::BPL, top);
    g.loop_depth--;
    ForgetRegs(g);
}

} // namespace

void CopyStruct(Gen& g, ExprId value, const PlaceRef& dst_in, TypeId type) {
    const Program& program = *g.program;
    PlaceRef dst = dst_in;
    PinPlace(g, dst, type);
    // read everything first whenever the source could overlap the destination
    bool snapshot = HasWrites(program, value) || HasLoads(program, value) || dst.pointer ||
                    dst.root < 0 || ReadsVar(program, value, dst.root);
    std::uint32_t size = TypeOf(program, type).size;
    std::vector<Opnd> values;
    bool loop = snapshot && size >= kMinLoopCopy && size <= kMaxLoopCopy && g.pin[1] < 0 &&
                !g.wide && ExprOf(program, value).kind != ExprKind::StructLit;
    if (!loop) {
        CollectValue(g, value, type, snapshot, values);
    } else {
        PlaceRef source;
        if (!ResolvePlace(g, value, -1, source)) {
            return;
        }
        NormalizePlace(g, source, type);
        if (LoopPlace(g, source) && LoopPlace(g, dst)) {
            LoopCopy(g, source, dst, size);
            return;
        }
        CollectPlace(g, source, type, snapshot, values);
    }
    std::vector<Leaf> leaves;
    Leaves(g, dst, type, leaves);
    if (values.size() != leaves.size()) {
        if (!g.failed) {
            Fail(g, ExprOf(program, value).loc, "internal: struct value and place differ");
        }
        return;
    }
    for (std::size_t i = 0; i < leaves.size(); i++) {
        CopyOpnd(g, values[i], PlaceOpnd(g, leaves[i].place, leaves[i].type));
    }
}

} // namespace tisple
