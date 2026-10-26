#include "check/checker.hpp"

namespace tisple {

namespace {

ExprId CheckArmIn(Checker& ck, const Node& node, TypeId hint, bool statement) {
    ck.at.scopes.emplace_back();
    ExprId id = statement ? CheckStatement(ck, node) : CheckExpr(ck, node, hint);
    ck.at.scopes.pop_back();
    return id;
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

TypeId CheckArms(Checker& ck, const std::vector<const Node*>& arms, TypeId hint,
                 std::vector<ExprId>& out, const Node& owner, const char* what) {
    out.assign(arms.size(), -1);
    TypeId joint = -1;
    bool any_void = false;
    bool reported = false;
    std::size_t joint_arm = 0;
    for (std::size_t i = 0; i < arms.size(); i++) {
        if (IsBareLiteral(*arms[i])) {
            continue;
        }
        out[i] = CheckArmIn(ck, *arms[i], hint, false);
        TypeId type = ExprType(ck, out[i]);
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
                fix = ", as in (" + ShowType(ck, widen_first ? type : joint) + " " +
                      Show(*arms[widen_first ? joint_arm : i]) + ")";
            }
            Err(ck, *arms[i],
                std::string(what) + " arms give " + ShowType(ck, joint) + " and " +
                    ShowType(ck, type) + "; make them the same type" + fix);
            reported = true;
        }
    }
    TypeId literal_type = joint >= 0 ? joint : hint;
    for (std::size_t i = 0; i < arms.size(); i++) {
        if (!IsBareLiteral(*arms[i])) {
            continue;
        }
        if (any_void) {
            out[i] = CheckArmIn(ck, *arms[i], -1, true);
            continue;
        }
        out[i] = CheckIntLiteral(ck, *arms[i], literal_type);
        if (joint < 0 && ExprType(ck, out[i]) > kErrorType) {
            joint = ExprType(ck, out[i]);
        }
    }
    (void)owner;
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

ExprId CheckMatch(Checker& ck, const Node& node, TypeId hint) {
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
    std::vector<const Node*> bodies;
    bool has_wildcard = false;
    bool ok = true;
    for (std::size_t i = 2; i < node.items.size(); i++) {
        const Node& clause_node = node.items[i];
        if (clause_node.kind != NodeKind::List || clause_node.items.size() != 2) {
            Err(ck, clause_node,
                "a match clause is (pattern form); group several forms with (do ...)");
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
        bodies.push_back(&clause_node.items[1]);
    }
    if (ok) {
        CheckCoverage(ck, node, type, clauses, has_wildcard);
    }
    std::vector<ExprId> out;
    TypeId result = CheckArms(ck, bodies, hint, out, node, "match");
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
