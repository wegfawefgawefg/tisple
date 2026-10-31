// (with_aux reads writes body ...): the body runs with RAMRD on when reads is true and RAMWRT
// on when writes is (docs/language.md, Aux memory code). The form becomes
//   (do on body ... off)
// with on an AuxSwitch that sets both switches from the bools and off one that turns both off
// after working out the body's last form, which is the form's value. Every return, and every
// break and continue to a loop outside, turns them off first: (return v) has off around its v,
// (return), (break) and (continue) an off before them. The rules for what runs inside are in
// aux_rules.cpp; where the switches may be on, check/aux_mode.hpp.

#include "check/aux_mode.hpp"
#include "check/checker.hpp"

namespace tisple {

namespace {

ExprId NewOff(Checker& ck, const Node& node, ExprId value, std::uint8_t bits) {
    TypeId type = value >= 0 ? ExprType(ck, value) : kVoidType;
    ExprId off = AddExpr(ck, ExprKind::AuxSwitch, type, node);
    Expr& e = ExprAt(ck, off);
    e.value = 0;
    e.a = value;
    e.ref = bits;
    return off;
}

ExprId NewDo(Checker& ck, const Node& node, const std::vector<ExprId>& forms, TypeId type) {
    ExprId id = AddExpr(ck, ExprKind::Do, type, node);
    ExprAt(ck, id).first = AddList(ck, forms);
    ExprAt(ck, id).count = static_cast<std::uint32_t>(forms.size());
    return id;
}

ExprId CheckSwitchValue(Checker& ck, const Node& node, const char* what) {
    ExprId value = CheckExpr(ck, node, kBoolType);
    RequireType(ck, node, value, kBoolType, std::string("with_aux's ") + what);
    return value;
}

} // namespace

ExprId CheckWithAux(Checker& ck, const Node& node, TypeId hint, bool used) {
    if (node.items.size() < 3) {
        return Fail(ck, node,
                    "with_aux takes whether it reads and whether it writes aux memory, then its "
                    "body; write (with_aux true false body ...)");
    }
    if (ck.at.function < 0) {
        return Fail(ck, node, "with_aux is only inside a fn");
    }
    if (ck.at.aux_depth >= 0) {
        return Fail(ck, node,
                    "with_aux inside with_aux: the inner one would turn the switches off before "
                    "the outer one ends; give the outer one the switches both need");
    }
    ExprId reads = CheckSwitchValue(ck, node.items[1], "reads");
    ExprId writes = CheckSwitchValue(ck, node.items[2], "writes");
    ExprId on = AddExpr(ck, ExprKind::AuxSwitch, kVoidType, node);
    ExprAt(ck, on).value = 1;
    ExprAt(ck, on).a = reads;
    ExprAt(ck, on).b = writes;
    std::uint8_t bits = AuxOnBits(*ck.out, ExprAt(ck, on));
    ExprAt(ck, on).ref = bits;

    int saved_depth = ck.at.aux_depth;
    std::uint8_t saved_bits = ck.at.aux_bits;
    ck.at.aux_depth = ck.at.loop_depth;
    ck.at.aux_bits = bits;
    ck.at.scopes.emplace_back();
    bool want = used && node.items.size() > 3;
    ExprId body = CheckBody(ck, node.items, 3, hint, want, node);
    ck.at.scopes.pop_back();
    ck.at.aux_depth = saved_depth;
    ck.at.aux_bits = saved_bits;
    if (used && node.items.size() == 3) {
        return Fail(ck, node, "this with_aux has no body to give a value; add one");
    }

    const Expr& b = ExprAt(ck, body);
    std::vector<ExprId> forms = {on};
    for (std::uint32_t i = 0; i < b.count; i++) {
        forms.push_back(ck.out->lists[b.first + i]);
    }
    ExprId value = -1;
    if (want) {
        value = forms.back();
        forms.pop_back();
    }
    ExprId off = NewOff(ck, node, value, bits);
    forms.push_back(off);
    return NewDo(ck, node, forms, ExprType(ck, off));
}

ExprId LeaveAux(Checker& ck, const Node& node, ExprId exit) {
    if (ck.at.aux_depth < 0) {
        return exit;
    }
    Expr& e = ExprAt(ck, exit);
    bool leaves = e.kind == ExprKind::Return || ck.at.loop_depth == ck.at.aux_depth;
    if (!leaves) {
        return exit; // a break or continue to a loop inside the region
    }
    if (e.kind == ExprKind::Return && e.a >= 0) {
        ExprId off = NewOff(ck, node, e.a, ck.at.aux_bits);
        ExprAt(ck, exit).a = off;
        return exit;
    }
    ExprId off = NewOff(ck, node, -1, ck.at.aux_bits);
    return NewDo(ck, node, {off, exit}, kNeverType);
}

} // namespace tisple
