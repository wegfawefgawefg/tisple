// Function pointers: (fn (u8 u8) u8) types, a function's name as a value (its address), and
// calls through a pointer, (f x y) with f a local, parameter, global or field that holds one.
// No conversion makes or reads a function pointer, so a call through one of type t reaches
// only functions whose address is taken with type t: those are its callees.

#include "check/checker.hpp"

namespace tisple {

TypeId FnType(Checker& ck, const std::vector<TypeId>& params, TypeId ret, const Node& at) {
    (void)at;
    std::vector<TypeId> key = params;
    key.push_back(ret);
    for (TypeId type : key) {
        if (type == kErrorType) {
            return kErrorType;
        }
    }
    std::int32_t index = -1;
    auto found = ck.signature_index.find(key);
    if (found != ck.signature_index.end()) {
        index = found->second;
    } else {
        index = static_cast<std::int32_t>(ck.out->signatures.size());
        ck.out->signatures.push_back(Signature{params, ret});
        ck.signature_index.emplace(key, index);
    }
    Type type;
    type.kind = TypeKind::Fn;
    type.decl = index;
    type.size = 2;
    return InternType(ck, type);
}

TypeId FunctionType(Checker& ck, std::int32_t function) {
    EnsureSignature(ck, function);
    Function& fn = ck.out->functions[static_cast<std::size_t>(function)];
    if (fn.type >= 0) {
        return fn.type;
    }
    std::vector<TypeId> params;
    for (std::int32_t param : fn.params) {
        params.push_back(ck.out->vars[static_cast<std::size_t>(param)].type);
    }
    Node none;
    TypeId type = FnType(ck, params, fn.ret, none);
    ck.out->functions[static_cast<std::size_t>(function)].type = type;
    return type;
}

ExprId FunctionValue(Checker& ck, const Node& node, std::int32_t function) {
    TypeId type = FunctionType(ck, function);
    if (type == kErrorType) {
        return ErrorExpr(ck, node);
    }
    if (ck.out->functions[static_cast<std::size_t>(function)].interrupt) {
        return Fail(ck, node,
                    node.text + " is the interrupt handler: an IRQ runs it, no pointer does");
    }
    ck.out->functions[static_cast<std::size_t>(function)].address_taken = true;
    ExprId id = AddExpr(ck, ExprKind::FnRef, type, node);
    ExprAt(ck, id).ref = function;
    return id;
}

ExprId CheckIndirectCall(Checker& ck, const Node& node, ExprId callee) {
    TypeId type = ExprType(ck, callee);
    const Signature sig = ck.out->signatures[static_cast<std::size_t>(TypeOf(*ck.out, type).decl)];
    const std::string& head = node.items[0].text;
    std::size_t given = node.items.size() - 1;
    if (given != sig.params.size()) {
        return Fail(ck, node,
                    head + " is a " + ShowType(ck, type) + ", which takes " +
                        std::to_string(sig.params.size()) + " argument" +
                        (sig.params.size() == 1 ? "" : "s") + ", got " + std::to_string(given));
    }
    std::vector<ExprId> args;
    bool ok = true;
    for (std::size_t i = 0; i < given; i++) {
        ExprId arg = CheckExpr(ck, node.items[i + 1], sig.params[i]);
        ok = RequireType(ck, node.items[i + 1], arg, sig.params[i],
                         "argument " + std::to_string(i + 1) + " of " + head) &&
             ok;
        args.push_back(arg);
    }
    if (ck.at.function >= 0) {
        ck.indirect_calls.push_back({ck.at.function, type});
    }
    ExprId id = AddExpr(ck, ExprKind::CallPtr, ok ? sig.ret : kErrorType, node);
    Expr& expr = ExprAt(ck, id);
    expr.a = callee;
    expr.first = AddList(ck, args);
    expr.count = static_cast<std::uint32_t>(args.size());
    return id;
}

void AddIndirectCallees(Checker& ck) {
    for (const auto& [function, type] : ck.indirect_calls) {
        std::vector<std::int32_t>& callees =
            ck.out->functions[static_cast<std::size_t>(function)].callees;
        for (std::int32_t target : CallTargets(*ck.out, type)) {
            bool known = false;
            for (std::int32_t callee : callees) {
                known = known || callee == target;
            }
            if (!known) {
                callees.push_back(target);
            }
        }
    }
}

} // namespace tisple
