// String literals in code: "HELLO" is a (ptr u8) to its ASCII bytes and a 0 after them. Each
// different text is one const table (named str#0, str#1, ... so no written name can meet
// it), shared by every use, laid out with the other constant tables when kept code reaches
// it. The tree is (addr_of (at table 0)), which every later stage already knows; nothing
// writes the table, so stores through such a pointer are undefined, as for any constant.

#include "check/checker.hpp"

namespace tisple {

std::int32_t StringTable(Checker& ck, const Node& node) {
    for (unsigned char c : node.bytes) {
        if (c >= 0x80) {
            Err(ck, node,
                "a string in code is plain ASCII (0 to 127); put other bytes in a const table");
            return -1;
        }
    }
    auto found = ck.string_tables.find(node.bytes);
    if (found != ck.string_tables.end()) {
        return found->second;
    }
    Var var;
    var.name = "str#" + std::to_string(ck.string_tables.size());
    var.kind = VarKind::Const;
    var.type = ArrayOf(ck, kU8Type, static_cast<std::uint32_t>(node.bytes.size() + 1), node);
    var.module = ck.at.module;
    var.loc = NodeLoc(ck, node);
    var.address_taken = true;
    var.init.assign(node.bytes.begin(), node.bytes.end());
    var.init.push_back(0);
    if (var.type == kErrorType) {
        return -1;
    }
    std::int32_t id = AddVar(ck, var);
    ck.string_tables.emplace(node.bytes, id);
    return id;
}

ExprId StringPointer(Checker& ck, const Node& node, std::int32_t table) {
    const Var& var = ck.out->vars[static_cast<std::size_t>(table)];
    ExprId ref = AddExpr(ck, ExprKind::VarRef, var.type, node);
    ExprAt(ck, ref).ref = table;
    TypeId index_type = TypeOf(*ck.out, var.type).count <= 256 ? kU8Type : kU16Type;
    ExprId zero = AddExpr(ck, ExprKind::Literal, index_type, node);
    ExprId first = AddExpr(ck, ExprKind::Index, kU8Type, node);
    ExprAt(ck, first).a = ref;
    ExprAt(ck, first).b = zero;
    ExprId id = AddExpr(ck, ExprKind::AddrOf, PtrType(ck, kU8Type), node);
    ExprAt(ck, id).a = first;
    return id;
}

ExprId CheckString(Checker& ck, const Node& node) {
    std::int32_t table = StringTable(ck, node);
    if (table < 0) {
        return ErrorExpr(ck, node);
    }
    return StringPointer(ck, node, table);
}

} // namespace tisple
