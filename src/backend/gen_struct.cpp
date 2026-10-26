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
    std::vector<Leaf> leaves;
    Leaves(g, source, type, leaves);
    for (const Leaf& leaf : leaves) {
        Opnd o = PlaceOpnd(g, leaf.place, leaf.type);
        out.push_back(snapshot ? Snapshot(g, o) : o);
    }
}

} // namespace

void CopyStruct(Gen& g, ExprId value, const PlaceRef& dst, TypeId type) {
    const Program& program = *g.program;
    // read everything first whenever the source could overlap the destination
    bool snapshot = HasWrites(program, value) || HasLoads(program, value) || dst.pointer ||
                    dst.root < 0 || ReadsVar(program, value, dst.root);
    std::vector<Opnd> values;
    CollectValue(g, value, type, snapshot, values);
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
