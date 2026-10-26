#pragma once

// shared by the check/*.cpp files; the public entry point is check/check.hpp

#include "check/load.hpp"
#include "check/program.hpp"

#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace tisple {

enum class DeclKind : std::uint8_t {
    Struct,
    Enum,
    Const,
    Var,
    Function,
    Macro,
};

struct Decl {
    DeclKind kind = DeclKind::Var;
    std::int32_t index = -1;
    Loc loc;
};

struct ModuleScope {
    std::unordered_map<std::string, Decl> names;
    std::unordered_map<std::string, std::int32_t> imports; // prefix -> module
};

enum class Progress : std::uint8_t {
    Pending,
    Busy,
    Done,
    Failed,
};

struct ConstDecl {
    std::string name;
    std::int32_t module = -1;
    const Node* form = nullptr;
    Progress state = Progress::Pending;
    TypeId type = kErrorType;
    std::uint16_t value = 0; // scalars
    std::int32_t var = -1;   // arrays
};

struct ScopeEntry {
    std::string name;
    std::int32_t var = -1;
};

struct Scope {
    std::vector<ScopeEntry> entries;
};

// what a (possibly dotted) name leads to, before any struct fields
enum class NameKind : std::uint8_t {
    None,
    Var,
    Const,
    Function,
    Struct,
    Enum,
    EnumMember,
    Module,
};

struct NameRef {
    NameKind kind = NameKind::None;
    std::int32_t index = -1;
    std::uint16_t value = 0; // enum member
    std::size_t used = 0;    // segments consumed
    std::string missing_in;  // set when a module or enum lacks the next segment
};

// where the checker is: saved and restored around lazy work in other modules
struct Context {
    std::int32_t module = 0;
    std::uint32_t file = 0;
    std::int32_t function = -1; // -1: constant context, no locals
    std::vector<Scope> scopes;
    int loop_depth = 0;
};

struct Checker {
    LoadedProgram* in = nullptr;
    Program* out = nullptr;
    std::vector<Diagnostic>* errors = nullptr;
    std::vector<ModuleScope> modules;
    std::vector<ConstDecl> consts;
    std::vector<const Node*> struct_forms;
    std::vector<Progress> struct_states;
    std::vector<const Node*> global_forms;   // by var id; null for non-globals
    std::vector<const Node*> function_forms; // by function id
    std::vector<std::size_t> body_starts;    // by function id
    std::map<std::tuple<int, int, std::uint32_t, int>, TypeId> interned;
    Context at;
};

// diagnostics and building
Loc NodeLoc(const Checker& ck, const Node& node);
void Err(Checker& ck, const Node& node, const std::string& message);
ExprId AddExpr(Checker& ck, ExprKind kind, TypeId type, const Node& node);
ExprId ErrorExpr(Checker& ck, const Node& node);
ExprId Fail(Checker& ck, const Node& node, const std::string& message);
Expr& ExprAt(Checker& ck, ExprId id);
TypeId ExprType(const Checker& ck, ExprId id);
std::uint32_t AddList(Checker& ck, const std::vector<ExprId>& items);
// every new Var goes through here so global_forms stays indexed by var id
std::int32_t AddVar(Checker& ck, const Var& var);
std::string Show(const Node& node);
std::string ShowType(const Checker& ck, TypeId type);
const std::string& HeadName(const Node& node);
bool IsBareLiteral(const Node& node);
Context SwitchContext(Checker& ck, std::int32_t module, std::int32_t function);
void RestoreContext(Checker& ck, Context saved);

// types (declare.cpp)
TypeId InternType(Checker& ck, const Type& type);
TypeId PtrType(Checker& ck, TypeId elem);
TypeId ArrayOf(Checker& ck, TypeId elem, std::uint32_t count, const Node& at);
TypeId ResolveType(Checker& ck, const Node& node);
// -1 when the node is not written as a type; otherwise ResolveType
TypeId TryResolveType(Checker& ck, const Node& node);
bool EnsureStruct(Checker& ck, std::int32_t decl, const Node& at);
void DeclareModule(Checker& ck, std::int32_t module);
NameRef LookupName(Checker& ck, const std::vector<std::string>& segments);
std::vector<std::string> SplitDotted(const std::string& text);
// naming checks for a new name; false (with an error) when it is not allowed
enum class NameRule : std::uint8_t {
    Snake,
    Type,
    Constant,
};
bool CheckNewName(Checker& ck, const Node& name, NameRule rule, const char* what);
// also no repeat in the innermost scope; what is "local", "parameter", "loop variable"
bool CheckLocalName(Checker& ck, const Node& name, const char* what);
// the TypeId of struct or enum declaration `decl`
TypeId StructType(Checker& ck, std::int32_t decl);
TypeId EnumType(Checker& ck, std::int32_t decl);

// constants (consts.cpp)
void EvalConst(Checker& ck, std::int32_t index);
bool FoldExpr(Checker& ck, ExprId id, std::uint16_t& value);
bool FoldInit(Checker& ck, ExprId id, std::vector<std::uint8_t>& bytes, std::uint32_t offset);
bool ConstCount(Checker& ck, const Node& node, std::uint32_t& count);
void ResolveGlobal(Checker& ck, std::int32_t var);

// expressions (expr.cpp)
ExprId CheckExpr(Checker& ck, const Node& node, TypeId hint);
ExprId CheckIntLiteral(Checker& ck, const Node& node, TypeId hint);
ExprId CheckStatement(Checker& ck, const Node& node);
// error unless expr has type want (Never and Error pass); `what` names the slot
bool RequireType(Checker& ck, const Node& node, ExprId expr, TypeId want, const std::string& what);
bool IsPlaceKind(ExprKind kind);
bool RequireStructSource(Checker& ck, const Node& node, ExprId expr);

// forms (forms.cpp)
// forms from items[first] on, as a Do, in the current scope (callers push one)
ExprId CheckBody(Checker& ck, const std::vector<Node>& items, std::size_t first, TypeId hint,
                 bool want_value, const Node& owner);
ExprId CheckLet(Checker& ck, const Node& node);
ExprId CheckSet(Checker& ck, const Node& node);
ExprId CheckIf(Checker& ck, const Node& node, TypeId hint);
ExprId CheckDo(Checker& ck, const Node& node, TypeId hint);
ExprId CheckWhile(Checker& ck, const Node& node);
ExprId CheckFor(Checker& ck, const Node& node);
ExprId CheckReturn(Checker& ck, const Node& node);
ExprId CheckLoopExit(Checker& ck, const Node& node, bool is_break);

// match (match.cpp)
ExprId CheckMatch(Checker& ck, const Node& node, TypeId hint);
// checks arms, deferring bare literals until a type is known; returns the joint type
TypeId CheckArms(Checker& ck, const std::vector<const Node*>& arms, TypeId hint,
                 std::vector<ExprId>& out, const Node& owner, const char* what);

// access and calls (access.cpp)
ExprId CheckAt(Checker& ck, const Node& node);
ExprId CheckDot(Checker& ck, const Node& node);
ExprId CheckLoad(Checker& ck, const Node& node);
ExprId CheckStore(Checker& ck, const Node& node);
ExprId CheckAddrOf(Checker& ck, const Node& node);
ExprId CheckAs(Checker& ck, const Node& node);
ExprId CheckIoRead(Checker& ck, const Node& node);
ExprId CheckIoWrite(Checker& ck, const Node& node);
ExprId CheckCall(Checker& ck, const Node& node, std::int32_t function);
ExprId CheckStructLit(Checker& ck, const Node& node, TypeId type);
ExprId FieldAccess(Checker& ck, const Node& node, ExprId base, const std::string& field);
// the Var a place is rooted at, or -1; through_load is set when it goes through a Load
std::int32_t PlaceRoot(Checker& ck, ExprId place, bool& through_load);

} // namespace tisple
