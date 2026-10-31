#include "check/checker.hpp"

namespace tisple {

namespace {

// an arm of one form is that form; none or several make a do
ExprId ArmExpr(Checker& ck, const Arm& arm, const std::vector<ExprId>& forms, TypeId type) {
    if (forms.size() == 1) {
        return forms[0];
    }
    ExprId id = AddExpr(ck, ExprKind::Do, type, *arm.owner);
    ExprAt(ck, id).first = AddList(ck, forms);
    ExprAt(ck, id).count = static_cast<std::uint32_t>(forms.size());
    return id;
}

// the forms before an arm's last, as statements, in the arm's scope (already open)
std::vector<ExprId> LeadingForms(Checker& ck, const Arm& arm) {
    std::vector<ExprId> forms;
    for (std::size_t i = arm.first; i + 1 < arm.end; i++) {
        forms.push_back(CheckStatement(ck, (*arm.items)[i]));
    }
    return forms;
}

// a dropped value: every form is a statement; never only when the arm cannot finish
ExprId CheckStatementArm(Checker& ck, const Arm& arm, TypeId& type) {
    ck.at.scopes.emplace_back();
    std::vector<ExprId> forms;
    for (std::size_t i = arm.first; i < arm.end; i++) {
        forms.push_back(CheckStatement(ck, (*arm.items)[i]));
    }
    ck.at.scopes.pop_back();
    type = kVoidType;
    if (!forms.empty() && ExprType(ck, forms.back()) == kNeverType) {
        type = kNeverType;
    }
    return ArmExpr(ck, arm, forms, type);
}

struct PatternResult {
    bool ok = false;
    bool wildcard = false;
    std::uint16_t value = 0;
};

PatternResult ReadPattern(Checker& ck, const Node& pattern, TypeId type) {
    PatternResult result;
    if (pattern.kind == NodeKind::Symbol && pattern.text == "_") {
        result.ok = true;
        result.wildcard = true;
        return result;
    }
    TypeKind kind = KindOf(*ck.out, type);
    if (kind == TypeKind::Enum) {
        const EnumDecl& ed = ck.out->enums[static_cast<std::size_t>(TypeOf(*ck.out, type).decl)];
        if (pattern.kind == NodeKind::Symbol && pattern.text.find('.') == std::string::npos) {
            for (const EnumMember& member : ed.members) {
                if (member.name == pattern.text) {
                    Err(ck, pattern,
                        "enum members are written qualified; write " + ed.name + "." +
                            pattern.text);
                    return result;
                }
            }
        }
        if (pattern.kind != NodeKind::Symbol) {
            Err(ck, pattern,
                "a pattern on " + ed.name + " is a member, like " + ed.name + "." +
                    ed.members[0].name + ", a constant, or _");
            return result;
        }
    } else if (pattern.kind != NodeKind::Integer && pattern.kind != NodeKind::Symbol) {
        Err(ck, pattern,
            "a pattern on " + ShowType(ck, type) + " is a number, a constant or _, not " +
                Show(pattern));
        return result;
    }
    ExprId expr = CheckExpr(ck, pattern, type);
    if (ExprType(ck, expr) == kErrorType) {
        return result;
    }
    if (!RequireType(ck, pattern, expr, type, "a pattern of this match")) {
        return result;
    }
    const Expr& literal = ExprAt(ck, expr);
    if (literal.kind != ExprKind::Literal) {
        Err(ck, pattern,
            Show(pattern) + " is not a constant; a pattern is a number, a constant, an enum "
                            "member or _");
        return result;
    }
    result.ok = true;
    result.value = static_cast<std::uint16_t>(literal.value);
    return result;
}

void CheckCoverage(Checker& ck, const Node& node, TypeId type, const std::vector<Clause>& clauses,
                   bool has_wildcard) {
    TypeKind kind = KindOf(*ck.out, type);
    if (kind != TypeKind::Enum) {
        if (!has_wildcard) {
            Err(ck, node,
                "a match on " + ShowType(ck, type) +
                    " needs a (_ ...) clause last, for the values no pattern names");
        }
        return;
    }
    const EnumDecl& ed = ck.out->enums[static_cast<std::size_t>(TypeOf(*ck.out, type).decl)];
    std::string missing;
    std::string clauses_fix;
    for (const EnumMember& member : ed.members) {
        bool named = false;
        for (const Clause& clause : clauses) {
            named = named || (!clause.wildcard && clause.value == member.value);
        }
        if (!named) {
            missing += (missing.empty() ? "" : ", ") + ed.name + "." + member.name;
            clauses_fix += (clauses_fix.empty() ? "" : " ") + std::string("(") + ed.name + "." +
                           member.name + " ...)";
        }
    }
    if (missing.empty() && has_wildcard) {
        Err(ck, node,
            "every member of " + ed.name + " is already matched; remove the (_ ...) clause");
    } else if (!missing.empty() && !has_wildcard) {
        Err(ck, node,
            "match on " + ed.name + " does not cover " + missing + "; add " + clauses_fix +
                " or a (_ ...) clause");
    }
}

} // namespace

TypeId CheckArms(Checker& ck, const std::vector<Arm>& arms, TypeId hint, bool used,
                 std::vector<ExprId>& out, const char* what) {
    out.assign(arms.size(), -1);
    if (!used) {
        bool all_never = !arms.empty();
        for (std::size_t i = 0; i < arms.size(); i++) {
            TypeId type = kVoidType;
            ck.at.dead += arms[i].dead ? 1 : 0;
            out[i] = CheckStatementArm(ck, arms[i], type);
            ck.at.dead -= arms[i].dead ? 1 : 0;
            all_never = all_never && type == kNeverType;
        }
        return all_never ? kNeverType : kVoidType;
    }

    // bare literals at the end of an arm wait for the type the other arms give
    std::vector<std::vector<ExprId>> waiting(arms.size());
    std::vector<bool> deferred(arms.size(), false);
    TypeId joint = -1;
    bool any_void = false;
    bool reported = false;
    std::size_t joint_arm = 0;
    for (std::size_t i = 0; i < arms.size(); i++) {
        const Arm& arm = arms[i];
        ck.at.scopes.emplace_back();
        ck.at.dead += arm.dead ? 1 : 0;
        std::vector<ExprId> forms = LeadingForms(ck, arm);
        ck.at.dead -= arm.dead ? 1 : 0;
        if (arm.end == arm.first) {
            ck.at.scopes.pop_back();
            out[i] = ArmExpr(ck, arm, forms, kVoidType);
            any_void = true;
            continue;
        }
        const Node& last = (*arm.items)[arm.end - 1];
        if (IsBareLiteral(last)) {
            ck.at.scopes.pop_back();
            waiting[i] = forms;
            deferred[i] = true;
            continue;
        }
        ck.at.dead += arm.dead ? 1 : 0;
        forms.push_back(CheckExpr(ck, last, hint));
        ck.at.dead -= arm.dead ? 1 : 0;
        ck.at.scopes.pop_back();
        TypeId type = ExprType(ck, forms.back());
        out[i] = ArmExpr(ck, arm, forms, type);
        if (type == kErrorType || type == kNeverType) {
            continue;
        }
        if (type == kVoidType) {
            any_void = true;
            continue;
        }
        if (joint < 0) {
            joint = type;
            joint_arm = i;
        } else if (type != joint && !reported) {
            // the fix widens the narrower arm
            std::string fix;
            TypeKind jk = KindOf(*ck.out, joint);
            TypeKind tk = KindOf(*ck.out, type);
            if (IsIntegerKind(jk) && IsIntegerKind(tk)) {
                bool widen_first = KindBits(jk) < KindBits(tk);
                const Arm& narrow = arms[widen_first ? joint_arm : i];
                fix = ", as in (as " + ShowType(ck, widen_first ? type : joint) + " " +
                      Show((*narrow.items)[narrow.end - 1]) + ")";
            }
            Err(ck, last,
                std::string(what) + " arms give " + ShowType(ck, joint) + " and " +
                    ShowType(ck, type) + "; make them the same type" + fix);
            reported = true;
        }
    }
    TypeId literal_type = joint >= 0 ? joint : hint;
    for (std::size_t i = 0; i < arms.size(); i++) {
        if (!deferred[i]) {
            continue;
        }
        const Arm& arm = arms[i];
        const Node& last = (*arm.items)[arm.end - 1];
        std::vector<ExprId>& forms = waiting[i];
        if (any_void) {
            forms.push_back(CheckStatement(ck, last));
            out[i] = ArmExpr(ck, arm, forms, kVoidType);
            continue;
        }
        forms.push_back(CheckIntLiteral(ck, last, literal_type));
        TypeId type = ExprType(ck, forms.back());
        out[i] = ArmExpr(ck, arm, forms, type);
        if (joint < 0 && type > kErrorType) {
            joint = type;
        }
    }
    if (reported) {
        return kErrorType;
    }
    if (any_void) {
        return kVoidType;
    }
    if (joint >= 0) {
        return joint;
    }
    for (ExprId arm : out) {
        if (ExprType(ck, arm) != kNeverType) {
            return kErrorType;
        }
    }
    return kNeverType;
}

ExprId CheckMatch(Checker& ck, const Node& node, TypeId hint, bool used) {
    if (node.items.size() < 3) {
        return Fail(ck, node, "match needs a value and clauses; write (match x (0 a) (_ b))");
    }
    ExprId scrutinee = CheckExpr(ck, node.items[1], -1);
    TypeId type = ExprType(ck, scrutinee);
    if (type == kErrorType) {
        return ErrorExpr(ck, node);
    }
    TypeKind kind = KindOf(*ck.out, type);
    if (kind == TypeKind::Bool) {
        return Fail(ck, node,
                    "match on a bool is an if; write (if " + Show(node.items[1]) + " a b)");
    }
    if (!IsIntegerKind(kind) && kind != TypeKind::Enum) {
        return Fail(ck, node, "match works on integers and enums, not " + ShowType(ck, type));
    }

    std::vector<Clause> clauses;
    std::vector<Arm> arms;
    bool has_wildcard = false;
    bool ok = true;
    for (std::size_t i = 2; i < node.items.size(); i++) {
        const Node& clause_node = node.items[i];
        if (clause_node.kind != NodeKind::List || clause_node.mark != QuoteMark::None ||
            clause_node.items.empty()) {
            Err(ck, clause_node, "a match clause is (pattern body ...), like (0 (f)) or (_)");
            ok = false;
            continue;
        }
        if (has_wildcard) {
            Err(ck, clause_node,
                "_ matches everything, so this clause never runs; move (_ ...) last");
            ok = false;
            continue;
        }
        PatternResult pattern = ReadPattern(ck, clause_node.items[0], type);
        if (!pattern.ok) {
            ok = false;
            continue;
        }
        bool repeated = false;
        for (const Clause& other : clauses) {
            repeated =
                repeated || (!other.wildcard && !pattern.wildcard && other.value == pattern.value);
        }
        if (repeated) {
            Err(ck, clause_node.items[0],
                Show(clause_node.items[0]) + " is matched twice; remove one clause");
            ok = false;
            continue;
        }
        has_wildcard = has_wildcard || pattern.wildcard;
        Clause clause;
        clause.wildcard = pattern.wildcard;
        clause.value = pattern.value;
        clause.loc = NodeLoc(ck, clause_node);
        clauses.push_back(clause);
        arms.push_back(Arm{&clause_node.items, 1, clause_node.items.size(), &clause_node});
    }
    if (ok) {
        CheckCoverage(ck, node, type, clauses, has_wildcard);
    }
    std::vector<ExprId> out;
    TypeId result = CheckArms(ck, arms, hint, used, out, "match");
    for (std::size_t i = 0; i < clauses.size(); i++) {
        clauses[i].body = out[i];
    }
    ExprId id = AddExpr(ck, ExprKind::Match, result, node);
    Expr& expr = ExprAt(ck, id);
    expr.a = scrutinee;
    expr.first = static_cast<std::uint32_t>(ck.out->clauses.size());
    expr.count = static_cast<std::uint32_t>(clauses.size());
    ck.out->clauses.insert(ck.out->clauses.end(), clauses.begin(), clauses.end());
    return id;
}

} // namespace tisple
