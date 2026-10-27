#pragma once

// The C writer's state, shared by its expression and statement halves (race_c.hpp).

#include "check/program.hpp"

#include <string>
#include <vector>

namespace tisple::racec {

// an expression as C: statements that must run first, then a side-effect-free value
struct CValue {
    std::string pre;
    std::string text;
    // nothing pre can do changes it: a literal, a temporary, a local or a parameter
    // (functions cannot reach the caller's locals)
    bool stable = false;
};

struct LoopCtx {
    int label = 0;     // brk<label>: after the loop, for a break inside a switch
    int switches = 0;  // switch depth when the loop started
    bool used = false; // the label was jumped to
};

struct Writer {
    const Program* program = nullptr;
    std::string error;
    // the function being written
    std::int32_t function = -1;
    std::vector<std::string> decls; // its locals and temporaries
    int temps = 0;
    int labels = 0;
    int indent = 1;
    int switches = 0;
    std::vector<LoopCtx> loops;
    std::vector<std::string> var_names; // C name per Var id
    std::vector<std::string> fn_names;  // C name per Function id

    const Expr& At(ExprId id) const;
    TypeKind Kind(TypeId type) const;
    bool Fail(const std::string& why);
    std::string Pad() const;
    std::string CType(TypeId type) const;
    // "uint8_t x", "uint8_t *p"
    std::string Declare(TypeId type, const std::string& name) const;
    std::string NewTemp(TypeId type);
    // a temporary holding v, unless it is stable already
    CValue Stabilize(CValue v, TypeId type);

    // ---- expressions (race_c_expr.cpp)
    std::string Literal(TypeId type, std::uint32_t raw) const;
    CValue Expr(ExprId id);
    // a place to read or write; stabilize: every index in it is evaluated now, so a
    // later call cannot change which element it is
    CValue Place(ExprId id, bool stabilize);
    CValue Call(const tisple::Expr& e, bool as_value);
    CValue Logic(const tisple::Expr& e);
    CValue IfValue(const tisple::Expr& e);
    CValue MatchValue(const tisple::Expr& e);
    CValue DoValue(const tisple::Expr& e);
    // two operands in order: a is kept in a temporary when b's statements could change it
    void Order(CValue& a, TypeId a_type, CValue& b);

    // ---- arithmetic (race_c_arith.cpp)
    CValue Binary(const tisple::Expr& e);
    CValue Shift(const tisple::Expr& e, CValue a, CValue b);
    CValue Divide(const tisple::Expr& e, CValue a, CValue b);

    // ---- statements (race_c.cpp)
    std::string Stmt(ExprId id);
    std::string Block(ExprId id);
    std::string Assign(ExprId place, TypeId type, ExprId value);
    std::string StoreStruct(const std::string& target, TypeId type, ExprId value);
    std::string IfStmt(const tisple::Expr& e);
    std::string WhileStmt(const tisple::Expr& e);
    std::string ForStmt(const tisple::Expr& e);
    // result: the temporary a match used as a value assigns, or "" for a statement
    std::string MatchStmt(const tisple::Expr& e, const std::string& result);
    std::string StoreStmt(const tisple::Expr& e);
    std::string StoreFieldStmt(const tisple::Expr& e);
    std::string LoopEnd(const LoopCtx& loop);
    std::string Params(std::int32_t index) const;
    std::string Prototype(std::int32_t index, const std::string& params) const;
    std::string Function(std::int32_t index);
};

std::string FieldName(const Program& program, TypeId struct_type, std::uint32_t offset);
bool IsIdentifier(const std::string& text);
// (load (+ p n)) as C: p[n] for a named pointer, else *(p + n)
std::string PointerAt(const std::string& pointer, const std::string& count);

} // namespace tisple::racec
