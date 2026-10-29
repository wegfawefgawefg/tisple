#pragma once

// shared by the check/*.cpp files; the public entry point is check/check.hpp

#include "check/load.hpp"
#include "check/name_index.hpp"
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
    std::int32_t var = -1;   // arrays, and a string's table
    bool string = false;     // a (ptr u8) to the string table var
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
    std::map<std::string, std::int32_t> string_tables; // a string's bytes -> its table var
    Context at;
    NameIndex* names = nullptr; // editor tools only: what each written name is
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

// editor index: segment `segment` of node is this; no-op without ck.names
void Note(Checker& ck, const Node& node, std::size_t segment, NameUseKind kind, std::int32_t target,
          std::int32_t member, bool declaration);
// field_expr came from FieldAccess: notes it when it is a Field
void NoteField(Checker& ck, const Node& node, std::size_t segment, ExprId field_expr);

// types (declare.cpp)
TypeId InternType(Checker& ck, const Type& type);
TypeId PtrType(Checker& ck, TypeId elem);
TypeId ArrayOf(Checker& ck, TypeId elem, std::uint32_t count, const Node& at);
TypeId ResolveType(Checker& ck, const Node& node);
// -1 when the node is not written as a type; otherwise ResolveType
TypeId TryResolveType(Checker& ck, const Node& node);
bool EnsureStruct(Checker& ck, std::int32_t decl, const Node& at);
void DeclareModule(Checker& ck, std::int32_t module);
// `at` is the symbol being resolved: names a macro template wrote resolve in the macro's module
NameRef LookupName(Checker& ck, const std::vector<std::string>& segments, const Node& at);
std::int32_t NameModule(const Checker& ck, const Node& at);
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

// placement options (placement.cpp). ReadOption reads the option at key and gives how many
// items it took; CheckRegionOptions checks :reserve and :in aux once the type is known;
// SplitOptions reads the options that end a const form into holder and leaves the rest in
// trimmed, giving the first option or null
int ReadOption(Checker& ck, Var& var, const Node& key, const Node* value);
void CheckRegionOptions(Checker& ck, const Node& form, const Var& var, bool has_value);
const Node* SplitOptions(Checker& ck, const Node& form, Node& trimmed, Var& holder);

// expressions (expr.cpp)
ExprId CheckExpr(Checker& ck, const Node& node, TypeId hint);
ExprId CheckIntLiteral(Checker& ck, const Node& node, TypeId hint);
ExprId CheckStatement(Checker& ck, const Node& node);
// error unless expr has type want (Never and Error pass); `what` names the slot
bool RequireType(Checker& ck, const Node& node, ExprId expr, TypeId want, const std::string& what);
bool IsPlaceKind(ExprKind kind);
bool RequireStructSource(Checker& ck, const Node& node, ExprId expr);

// operators and conversions (operators.cpp)
bool FindBinaryOp(const std::string& name, Op& op);
void LiteralRange(TypeKind kind, std::int64_t& low, std::int64_t& high);
bool LiteralFits(TypeKind kind, std::int64_t value);
ExprId CheckBinary(Checker& ck, const Node& node, Op op, TypeId hint);
ExprId CheckUnary(Checker& ck, const Node& node, TypeId hint);
ExprId CheckLogic(Checker& ck, const Node& node);

// forms (forms.cpp)
// forms from items[first] on, as a Do, in the current scope (callers push one)
ExprId CheckBody(Checker& ck, const std::vector<Node>& items, std::size_t first, TypeId hint,
                 bool want_value, const Node& owner);
ExprId CheckLet(Checker& ck, const Node& node);
ExprId CheckSet(Checker& ck, const Node& node);
// used: the value is wanted; an if, match or do whose value is dropped is a statement, so
// its arms need not agree on a type
ExprId CheckIf(Checker& ck, const Node& node, TypeId hint, bool used);
ExprId CheckDo(Checker& ck, const Node& node, TypeId hint, bool used);
ExprId CheckWhile(Checker& ck, const Node& node);
ExprId CheckFor(Checker& ck, const Node& node);
ExprId CheckReturn(Checker& ck, const Node& node);
ExprId CheckLoopExit(Checker& ck, const Node& node, bool is_break);

// match (match.cpp)
ExprId CheckMatch(Checker& ck, const Node& node, TypeId hint, bool used);
// one arm of an if or match: the forms items[first, end); its value is the last
struct Arm {
    const std::vector<Node>* items = nullptr;
    std::size_t first = 0;
    std::size_t end = 0;
    const Node* owner = nullptr; // where errors about the whole arm point
};
// checks arms, each in its own scope. Used: bare literals wait until a type is known and the
// arms must agree; returns the joint type. Not used: every form is a statement; returns void
// (never when no arm finishes)
TypeId CheckArms(Checker& ck, const std::vector<Arm>& arms, TypeId hint, bool used,
                 std::vector<ExprId>& out, const char* what);

// access and calls (access.cpp; load, store, addr_of and as in pointers.cpp)
ExprId CheckAt(Checker& ck, const Node& node);
ExprId CheckDot(Checker& ck, const Node& node);
ExprId CheckLoad(Checker& ck, const Node& node);
ExprId CheckStore(Checker& ck, const Node& node);
ExprId CheckAddrOf(Checker& ck, const Node& node);
ExprId CheckAs(Checker& ck, const Node& node);
// (make Type ...): struct literals; array literals only as const and top-level let values
ExprId CheckMake(Checker& ck, const Node& node);
// a const or top-level let value of array type: (make (array t n) v ...) folded into bytes
bool FoldArrayLiteral(Checker& ck, const Node& value, TypeId type, const std::string& name,
                      std::vector<std::uint8_t>& bytes, std::uint32_t offset);
ExprId CheckIoRead(Checker& ck, const Node& node);
ExprId CheckIoWrite(Checker& ck, const Node& node);
// (jsr addr (a v) (x v) (y v)): a machine-code call; its value is A afterwards
ExprId CheckJsr(Checker& ck, const Node& node);
// string literals (strings.cpp): the const table holding the bytes and a 0, shared by equal
// strings (-1, with an error, when a byte is not ASCII); the (ptr u8) to it
std::int32_t StringTable(Checker& ck, const Node& node);
ExprId StringPointer(Checker& ck, const Node& node, std::int32_t table);
ExprId CheckString(Checker& ck, const Node& node);
// (wait_cycles n): an exact delay; a constant n of 2 or more, or any u16
ExprId CheckWaitCycles(Checker& ck, const Node& node);
ExprId CheckCall(Checker& ck, const Node& node, std::int32_t function);
// (make Sprite :x 1 ...): the fields are items[2..]
ExprId CheckStructLit(Checker& ck, const Node& node, TypeId type);
ExprId FieldAccess(Checker& ck, const Node& node, ExprId base, const std::string& field);
// the Var a place is rooted at, or -1; through_load is set when it goes through a Load
std::int32_t PlaceRoot(Checker& ck, ExprId place, bool& through_load);
// after every body: a :reserve let is never named in code, and a let :in aux only as the
// root of an addr_of
void CheckRegionUses(Checker& ck);

} // namespace tisple
