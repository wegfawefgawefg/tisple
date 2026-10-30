#include "backend/dispatch.hpp"

#include <map>

namespace tisple {

namespace {

ExprId AddNode(Program& program, const Expr& e) {
    program.exprs.push_back(e);
    return static_cast<ExprId>(program.exprs.size() - 1);
}

ExprId AddListNode(Program& program, Expr e, const std::vector<ExprId>& items) {
    e.first = static_cast<std::uint32_t>(program.lists.size());
    e.count = static_cast<std::uint32_t>(items.size());
    program.lists.insert(program.lists.end(), items.begin(), items.end());
    return AddNode(program, e);
}

ExprId VarNode(Program& program, std::int32_t var) {
    Expr e;
    e.kind = ExprKind::VarRef;
    e.type = program.vars[static_cast<std::size_t>(var)].type;
    e.ref = var;
    return AddNode(program, e);
}

std::int32_t AddParam(Program& program, std::int32_t function, const std::string& name,
                      TypeId type) {
    Var var;
    var.name = name;
    var.kind = VarKind::Param;
    var.type = type;
    var.module = program.functions[static_cast<std::size_t>(function)].module;
    var.function = function;
    std::int32_t id = static_cast<std::int32_t>(program.vars.size());
    program.vars.push_back(var);
    program.functions[static_cast<std::size_t>(function)].params.push_back(id);
    return id;
}

std::int32_t AddFunction(Program& program, const std::string& name, std::int32_t module,
                         TypeId ret) {
    Function fn;
    fn.name = name;
    fn.module = module;
    fn.ret = ret;
    program.functions.push_back(fn);
    return static_cast<std::int32_t>(program.functions.size() - 1);
}

struct Lowering {
    Program* program = nullptr;
    std::map<TypeId, std::int32_t> dispatchers;
    std::vector<std::int32_t> entries; // by original function
    std::vector<bool> seen;            // by expr: rewritten already (nodes may be shared)
};

// the dispatcher of type: the pointer, then the signature's parameters
std::int32_t Dispatcher(Lowering& lw, TypeId type) {
    auto found = lw.dispatchers.find(type);
    if (found != lw.dispatchers.end()) {
        return found->second;
    }
    Program& program = *lw.program;
    const Signature sig = program.signatures[static_cast<std::size_t>(TypeOf(program, type).decl)];
    std::int32_t d =
        AddFunction(program, "dispatch " + TypeName(program, type), program.entry_module, sig.ret);
    program.functions[static_cast<std::size_t>(d)].dispatcher = true;
    AddParam(program, d, "pointer", type);
    for (std::size_t i = 0; i < sig.params.size(); i++) {
        AddParam(program, d, "a" + std::to_string(i), sig.params[i]);
    }
    lw.dispatchers.emplace(type, d);
    return d;
}

// f.indirect: (f a0 a1 ...) with the dispatcher's parameters
std::int32_t MakeEntry(Lowering& lw, std::int32_t function) {
    Program& program = *lw.program;
    const Function fn = program.functions[static_cast<std::size_t>(function)];
    std::int32_t d = Dispatcher(lw, fn.type);
    std::int32_t entry = AddFunction(program, fn.name + ".indirect", fn.module, fn.ret);
    std::vector<ExprId> args;
    const std::vector<std::int32_t> params = program.functions[static_cast<std::size_t>(d)].params;
    for (std::size_t i = 1; i < params.size(); i++) {
        args.push_back(VarNode(program, params[i]));
    }
    Expr call;
    call.kind = ExprKind::Call;
    call.type = fn.ret;
    call.ref = function;
    call.loc = fn.loc;
    ExprId called = AddListNode(program, call, args);
    Expr body;
    body.kind = ExprKind::Do;
    body.type = fn.ret;
    body.loc = fn.loc;
    Function& e = program.functions[static_cast<std::size_t>(entry)];
    e.loc = fn.loc;
    e.callees.push_back(function);
    ExprId done = AddListNode(program, body, {called});
    program.functions[static_cast<std::size_t>(entry)].body = done;
    return entry;
}

// each dispatcher's body, for the analyses: a match on the pointer whose clauses call one
// entry each
void FinishDispatchers(Lowering& lw) {
    Program& program = *lw.program;
    std::size_t original = lw.entries.size();
    for (const auto& [type, d] : lw.dispatchers) {
        std::vector<std::int32_t> entries;
        for (std::size_t f = 0; f < original; f++) {
            if (lw.entries[f] >= 0 && program.functions[f].type == type) {
                entries.push_back(lw.entries[f]);
            }
        }
        TypeId ret = program.functions[static_cast<std::size_t>(d)].ret;
        Expr body;
        body.kind = ExprKind::Do;
        body.type = entries.empty() ? kVoidType : ret;
        std::vector<ExprId> items;
        if (!entries.empty()) {
            Expr match;
            match.kind = ExprKind::Match;
            match.type = ret;
            match.a = VarNode(program, program.functions[static_cast<std::size_t>(d)].params[0]);
            match.first = static_cast<std::uint32_t>(program.clauses.size());
            match.count = static_cast<std::uint32_t>(entries.size());
            for (std::size_t i = 0; i < entries.size(); i++) {
                Expr call;
                call.kind = ExprKind::Call;
                call.type = ret;
                call.ref = entries[i];
                Clause clause;
                clause.wildcard = i + 1 == entries.size();
                clause.value = static_cast<std::uint16_t>(i);
                clause.body = AddNode(program, call);
                program.clauses.push_back(clause);
            }
            items.push_back(AddNode(program, match));
        }
        ExprId done = AddListNode(program, body, items);
        Function& fn = program.functions[static_cast<std::size_t>(d)];
        fn.body = done;
        fn.callees = entries;
    }
}

void AddCallee(Program& program, std::int32_t function, std::int32_t callee) {
    std::vector<std::int32_t>& callees =
        program.functions[static_cast<std::size_t>(function)].callees;
    for (std::int32_t c : callees) {
        if (c == callee) {
            return;
        }
    }
    callees.push_back(callee);
}

// (p a b) into (dispatcher p a b), a function's name into its entry's, under id
void Rewrite(Lowering& lw, std::int32_t owner, ExprId id) {
    if (id < 0 || lw.seen[static_cast<std::size_t>(id)]) {
        return;
    }
    lw.seen[static_cast<std::size_t>(id)] = true;
    Program& program = *lw.program;
    Expr e = program.exprs[static_cast<std::size_t>(id)];
    if (e.kind == ExprKind::FnRef) {
        program.exprs[static_cast<std::size_t>(id)].ref =
            lw.entries[static_cast<std::size_t>(e.ref)];
        return;
    }
    if (e.kind == ExprKind::CallPtr) {
        std::int32_t d = Dispatcher(lw, program.exprs[static_cast<std::size_t>(e.a)].type);
        std::vector<ExprId> args = {e.a};
        for (std::uint32_t i = 0; i < e.count; i++) {
            args.push_back(program.lists[e.first + i]);
        }
        Expr call = e;
        call.kind = ExprKind::Call;
        call.ref = d;
        call.a = -1;
        call.first = static_cast<std::uint32_t>(program.lists.size());
        call.count = static_cast<std::uint32_t>(args.size());
        program.lists.insert(program.lists.end(), args.begin(), args.end());
        program.exprs[static_cast<std::size_t>(id)] = call;
        AddCallee(program, owner, d);
        e = call;
    }
    Rewrite(lw, owner, e.a);
    Rewrite(lw, owner, e.b);
    Rewrite(lw, owner, e.c);
    if (e.kind == ExprKind::Call || e.kind == ExprKind::StructLit || e.kind == ExprKind::Do) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            Rewrite(lw, owner, program.lists[e.first + i]);
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            Rewrite(lw, owner, program.clauses[e.first + i].body);
        }
    }
}

} // namespace

std::vector<std::int32_t> LowerIndirectCalls(Program& program) {
    Lowering lw;
    lw.program = &program;
    std::size_t original = program.functions.size();
    lw.entries.assign(original, -1);
    for (std::size_t f = 0; f < original; f++) {
        if (program.functions[f].address_taken) {
            lw.entries[f] = MakeEntry(lw, static_cast<std::int32_t>(f));
        }
    }
    lw.seen.assign(program.exprs.size(), false);
    for (std::size_t f = 0; f < original; f++) {
        Rewrite(lw, static_cast<std::int32_t>(f), program.functions[f].body);
    }
    FinishDispatchers(lw);
    return lw.entries;
}

void GenDispatcher(Backend& be, std::int32_t function) {
    const Program& program = *be.program;
    const Function& fn = program.functions[static_cast<std::size_t>(function)];
    std::vector<AsmItem>& out = be.code[static_cast<std::size_t>(function)];
    std::int32_t pointer = be.var_sym[static_cast<std::size_t>(fn.params[0])];
    std::int32_t jump = NewSym(be, fn.name + ".jump", SymRole::Label, function);
    out.push_back(MakeLabel(be.fn_sym[static_cast<std::size_t>(function)]));
    out.push_back(MakeIns(Mn::LDA, Mode::Abs, SymValue(pointer)));
    out.push_back(MakeIns(Mn::STA, Mode::Abs, SymValue(jump, 1)));
    out.push_back(MakeIns(Mn::LDA, Mode::Abs, SymValue(pointer, 1)));
    out.push_back(MakeIns(Mn::STA, Mode::Abs, SymValue(jump, 2)));
    out.push_back(MakeLabel(jump));
    // the entry the pointer holds; it returns straight to the dispatcher's caller
    out.push_back(MakeIns(Mn::JMP, Mode::Abs, Num(0)));
}

} // namespace tisple
