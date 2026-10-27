#include "race/race_gen.hpp"

// the crude shape signature of a race program: features of t.run and its callees

namespace tisple {

namespace {

struct ShapeWalk {
    const Program* program = nullptr;
    const std::vector<std::uint32_t>* counts = nullptr;
    std::map<std::string, std::uint64_t>* out = nullptr;
    std::uint64_t weight = 1; // of the expr being walked

    const Expr& At(ExprId id) const {
        return program->exprs[static_cast<std::size_t>(id)];
    }
    std::string Kind(TypeId type) const {
        TypeKind kind = KindOf(*program, type);
        switch (kind) {
        case TypeKind::Bool:
            return "bool";
        case TypeKind::U8:
            return "u8";
        case TypeKind::I8:
            return "i8";
        case TypeKind::U16:
            return "u16";
        case TypeKind::I16:
            return "i16";
        case TypeKind::Enum:
            return "enum";
        case TypeKind::Ptr:
            return "ptr";
        case TypeKind::Struct:
            return "struct";
        case TypeKind::Array:
            return "array";
        default:
            return "void";
        }
    }
    void Add(const std::string& name) {
        if (weight > 0) {
            (*out)[name] += weight;
        }
    }
    void Walk(ExprId id);
    void Binary(const Expr& e);
};

const char* ArithName(Op op) {
    switch (op) {
    case Op::Add:
    case Op::Sub:
        return "+-";
    case Op::Mul:
        return "*";
    case Op::Div:
        return "/";
    case Op::Mod:
        return "%";
    case Op::And:
    case Op::Or:
    case Op::Xor:
        return "&|^";
    case Op::Shl:
        return "<<";
    case Op::Shr:
        return ">>";
    default:
        return "cmp";
    }
}

void ShapeWalk::Binary(const Expr& e) {
    std::string name = std::string(ArithName(e.op)) + " " + Kind(At(e.a).type);
    bool literal = At(e.b).kind == ExprKind::Literal;
    if (e.op == Op::Shl || e.op == Op::Shr || e.op == Op::Mul || e.op == Op::Div ||
        e.op == Op::Mod) {
        name += literal ? " by const" : " by var";
    }
    Add(name);
}

void ShapeWalk::Walk(ExprId id) {
    if (id < 0) {
        return;
    }
    const Expr& e = At(id);
    weight = counts == nullptr ? 1 : (*counts)[static_cast<std::size_t>(id)];
    switch (e.kind) {
    case ExprKind::Binary:
        Binary(e);
        break;
    case ExprKind::Unary:
        Add(e.op == Op::Not ? "not" : "~ " + Kind(e.type));
        break;
    case ExprKind::And:
    case ExprKind::Or:
        Add("and/or");
        break;
    case ExprKind::Index:
        Add("at " + Kind(e.type) + "[] by " + Kind(At(e.b).type));
        break;
    case ExprKind::Field:
        Add("struct field");
        break;
    case ExprKind::Load:
    case ExprKind::Store:
        Add("load/store " + Kind(TypeOf(*program, At(e.a).type).elem));
        break;
    case ExprKind::LoadField:
    case ExprKind::StoreField:
        Add("struct pointer");
        break;
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
        Add("pointer +-");
        break;
    case ExprKind::AddrOf:
        Add("addr_of");
        break;
    case ExprKind::Convert:
        Add("as " + Kind(At(e.a).type) + " " + Kind(e.type));
        break;
    case ExprKind::Call:
        Add("call");
        break;
    case ExprKind::If:
        Add(e.type == kVoidType || e.type == kNeverType ? "if" : "if value");
        break;
    case ExprKind::Match:
        Add("match " + Kind(At(e.a).type) + (e.type == kVoidType ? "" : " value"));
        break;
    case ExprKind::While:
        Add("while");
        break;
    case ExprKind::For:
        Add("for " + Kind(program->vars[static_cast<std::size_t>(e.ref)].type));
        break;
    case ExprKind::Return:
        Add("return");
        break;
    case ExprKind::Break:
    case ExprKind::Continue:
        Add("break/continue");
        break;
    default:
        break;
    }
    Walk(e.a);
    Walk(e.b);
    Walk(e.c);
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            Walk(program->clauses[e.first + i].body);
        }
        return;
    }
    bool listed =
        e.kind == ExprKind::Do || e.kind == ExprKind::Call || e.kind == ExprKind::StructLit;
    for (std::uint32_t i = 0; listed && i < e.count; i++) {
        Walk(program->lists[e.first + i]);
    }
}

} // namespace

std::map<std::string, std::uint64_t> RaceFeatures(const Program& program,
                                                  const std::vector<std::uint32_t>* counts) {
    std::map<std::string, std::uint64_t> out;
    std::int32_t run = FindFunction(program, "t.run");
    if (run < 0) {
        return out;
    }
    // run and every function it reaches
    std::vector<bool> reached(program.functions.size(), false);
    std::vector<std::int32_t> todo = {run};
    reached[static_cast<std::size_t>(run)] = true;
    while (!todo.empty()) {
        std::int32_t f = todo.back();
        todo.pop_back();
        for (std::int32_t callee : program.functions[static_cast<std::size_t>(f)].callees) {
            if (!reached[static_cast<std::size_t>(callee)]) {
                reached[static_cast<std::size_t>(callee)] = true;
                todo.push_back(callee);
            }
        }
    }
    ShapeWalk walk;
    walk.program = &program;
    walk.counts = counts;
    walk.out = &out;
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (reached[f]) {
            walk.Walk(program.functions[f].body);
        }
    }
    return out;
}

} // namespace tisple
