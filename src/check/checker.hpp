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
    std::int32_t var = -1;   // structs and arrays: the Const var holding the bytes
    // a pointer whose value is an address the link knows ("HELLO", (addr_of T ...)): the
    // checked value, copied to every use (CloneExpr), else -1
    ExprId address = -1;
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
    // inside a with_aux: the loop depth where it started (a break or continue to a loop
    // outside leaves it), else -1; and the switches it may turn on
    int aux_depth = -1;
    std::uint8_t aux_bits = 0;
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
    // function pointers (fnptr.cpp): signatures by their types (the return type last), each
    // function's signature as ReadSignature reads it (lazily for a const table that names it),
    // and each indirect call's function and pointer type, for the callees
    std::map<std::vector<TypeId>, std::int32_t> signature_index;
    std::vector<Progress> signature_states;
    std::vector<std::pair<std::int32_t, TypeId>> indirect_calls;
    // the address slots of the value being folded (FoldValue), for its var
    std::vector<AddrSlot> pending_slots;
    // the Const vars whose values are being folded, innermost last: their addresses may be
    // used, their bytes not yet
    std::vector<std::int32_t> folding;
    // by var id: where each top-level let's resolution is (ResolveGlobal runs on first use)
    std::vector<Progress> global_states;
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
// where the new name is used, which decides the built-in names it may take
enum class NameSlot : std::uint8_t {
    // functions, types, constants: a function's name heads its calls, so no built-in name
    Declared,
    // lets, parameters, loop variables: a built-in name only heads a form, so they may take
    // one (at, load, make, ...), but not a type's, true, false or _
    Variable,
    // fields and enum members: always after a dot or a :keyword, so any name
    Field,
};
bool CheckNewName(Checker& ck, const Node& name, NameRule rule, NameSlot slot, const char* what);
// a variable holding a function pointer is called as (name args ...), so it may not take a
// built-in name, which would head the built-in instead
bool CheckCallableName(Checker& ck, const Node& name, TypeId type, const char* what);
// also no repeat in the innermost scope; what is "local", "parameter", "loop variable"
bool CheckLocalName(Checker& ck, const Node& name, const char* what);
// the TypeId of struct or enum declaration `decl`
TypeId StructType(Checker& ck, std::int32_t decl);
TypeId EnumType(Checker& ck, std::int32_t decl);

// constants (consts.cpp)
void EvalConst(Checker& ck, std::int32_t index);
bool FoldExpr(Checker& ck, ExprId id, std::uint16_t& value);
bool ConstCount(Checker& ck, const Node& node, std::uint32_t& count);
void ResolveGlobal(Checker& ck, std::int32_t var);
// a top-level let's type and value, resolved on its first use (a const may take its address
// before the lets are gone through in order)
void EnsureGlobal(Checker& ck, std::int32_t var);

// placement options (placement.cpp). ReadOption reads the option at key and gives how many
// items it took; CheckRegionOptions checks :reserve and :in aux once the type is known;
// SplitOptions reads the options that end a const form into holder and leaves the rest in
// trimmed, giving the first option or null
int ReadOption(Checker& ck, Var& var, const Node& key, const Node* value);
void CheckRegionOptions(Checker& ck, const Node& form, const Var& var, bool has_value);
const Node* SplitOptions(Checker& ck, const Node& form, Node& trimmed, Var& holder);

// const tables :on disk (disk_consts.cpp). MoveToDisk takes a folded table's bytes into
// Program::disk_tables and leaves its var the record's size; PlaceDiskConsts, once every const
// is folded, puts the tables on the disk in declaration order and writes their records
bool MoveToDisk(Checker& ck, ConstDecl& cd, const Node& option);
void PlaceDiskConsts(Checker& ck);

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
// (make Sprite :x 1 ...): the value node of each field in declared order; false (with an
// error) when a field is missing, repeated, unknown or out of order
bool OrderStructFields(Checker& ck, const Node& node, TypeId type,
                       std::vector<const Node*>& ordered);

// static data (data.cpp): the value of a const or a top-level let, node of `type`, into
// bytes at offset, its addresses into ck.pending_slots; false with an error. `what` names
// the slot in errors
bool FoldValue(Checker& ck, const Node& node, TypeId type, const std::string& what,
               std::vector<std::uint8_t>& bytes, std::uint32_t offset);
// a checked scalar value into bytes at offset: a constant, or an address slot for a pointer
// or function pointer; false when it is neither (an error only when folding found one)
bool FoldScalarValue(Checker& ck, ExprId value, TypeId type, std::vector<std::uint8_t>& bytes,
                     std::uint32_t offset);
// a pointer or function pointer whose value the link knows: the slot (its offset left 0)
bool FoldAddress(Checker& ck, ExprId id, AddrSlot& slot);
// a top-level let's or const's name as written (T for main.T), "the string" for a string's
std::string DataName(const Checker& ck, std::int32_t var);
// a copy of a checked expression tree (a constant pointer's value), its nodes at `at`
ExprId CloneExpr(Checker& ck, ExprId id, const Node& at);
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

// function pointers (fnptr.cpp)
// (fn (u8 u8) u8): the interned type
TypeId FnType(Checker& ck, const std::vector<TypeId>& params, TypeId ret, const Node& at);
// the function's (fn ...) type; reads its signature first if it has not been read
TypeId FunctionType(Checker& ck, std::int32_t function);
// a function's name as a value: its address, which counts as taking it
ExprId FunctionValue(Checker& ck, const Node& node, std::int32_t function);
// (f args ...) with f a name whose value is a function pointer, already checked as callee
ExprId CheckIndirectCall(Checker& ck, const Node& node, ExprId callee);
// after every body: each indirect call's function calls every function whose address is
// taken with its pointer's type
void AddIndirectCallees(Checker& ck);
// check.cpp: the signature, once (const tables may need it before the functions are read)
void EnsureSignature(Checker& ck, std::int32_t function);

// the interrupt handler (interrupts.cpp)
// (sei) and (cli)
ExprId CheckInterruptsForm(Checker& ck, const Node& node);
// once main is known: the handler shares no function with main and does not turn interrupts
// on; marks what it reads and writes (Var::irq_shared)
void CheckHandler(Checker& ck);

// aux memory code (with_aux.cpp, aux_rules.cpp)
// (with_aux reads writes body ...): [on, body ..., off], the value the body's last form's
ExprId CheckWithAux(Checker& ck, const Node& node, TypeId hint, bool used);
// a return, break or continue made inside a with_aux: an off first when it leaves the region
ExprId LeaveAux(Checker& ck, const Node& node, ExprId exit);
// once every body is checked: what code that runs with the switches on may not do
void CheckAuxCode(Checker& ck);
// (make Sprite :x 1 ...): the fields are items[2..]
ExprId CheckStructLit(Checker& ck, const Node& node, TypeId type);
ExprId FieldAccess(Checker& ck, const Node& node, ExprId base, const std::string& field);
// the Var a place is rooted at, or -1; through_load is set when it goes through a Load
std::int32_t PlaceRoot(Checker& ck, ExprId place, bool& through_load);
// after every body: a :reserve let is never named in code, and a let :in aux only as the
// root of an addr_of
void CheckRegionUses(Checker& ck);

} // namespace tisple
