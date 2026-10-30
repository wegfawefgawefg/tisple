#pragma once

// The fuzz generator's state, shared by its expression and statement halves.

#include <cstdint>
#include <string>
#include <vector>

namespace tisple_tests::fuzz {

enum class T : std::uint8_t { U8, I8, U16, I16, Bool, Enum };

inline const T kInts[] = {T::U8, T::I8, T::U16, T::I16};

inline const char* Name(T t) {
    switch (t) {
    case T::U8:
        return "u8";
    case T::I8:
        return "i8";
    case T::U16:
        return "u16";
    case T::I16:
        return "i16";
    case T::Bool:
        return "bool";
    default:
        return "E";
    }
}

inline bool IsInt(T t) {
    return t == T::U8 || t == T::I8 || t == T::U16 || t == T::I16;
}

struct Rng {
    std::uint64_t s = 1;

    std::uint32_t Next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<std::uint32_t>(s >> 11);
    }
    int Below(int n) {
        return n <= 0 ? 0 : static_cast<int>(Next() % static_cast<std::uint32_t>(n));
    }
    bool Chance(int percent) {
        return Below(100) < percent;
    }
};

struct VarInfo {
    std::string name;
    T type = T::U8;
    bool read_only = false;
    bool small = false; // a u16 that stays below 256: an index let
};

struct ArrayInfo {
    std::string name;
    T elem = T::U8;
    std::uint32_t count = 0;
    bool is_const = false;
    bool pointers = false; // addr_of reaches into it
};

struct FnInfo {
    std::string name;
    std::vector<T> params;
    bool has_ret = false;
    T ret = T::U8;
};

struct PtrInfo {
    std::string name;
    T elem = T::U8;
    bool to_struct = false;
    std::size_t depth = 0; // the scope it was declared in
    bool room = false;     // into wb with 510 bytes after it: any byte count stays inside
};

// struct S (a u8) (b i16) (c bool) (d u8)
inline const char* const kFields[] = {"a", "b", "c", "d"};
inline const T kFieldTypes[] = {T::U8, T::I16, T::Bool, T::U8};
constexpr int kEnumMembers = 5;

struct Gen {
    Rng rng;
    // for shapes added later: its own stream, so the programs of seeds that don't use them
    // stay what they were (the fuzz farm's seeds keep their programs)
    Rng side;
    // constant chains, as unrolled copies leave them: (+ (+ x 5) 3), (* (* x 3) 0)
    bool fold_shapes = false;
    // an operation on two byte-table reads with computed indices, calls in them now and then
    bool table_shapes = false;
    // loops with variables the backend may keep in registers or step pointers to, from a
    // stream of their own (fuzz_gen_loops.cpp)
    Rng loops_rng;
    bool loop_shapes = false;
    // loops entered by a jump, from a stream of their own (fuzz_gen_entries.cpp); only in
    // GenerateEntryProgram's programs, so GenerateProgram's stay what they were
    Rng entry_rng;
    bool entry_shapes = false;
    // waits and string reads, from a stream of their own (fuzz_gen_late.cpp); only in
    // GenerateLateProgram's programs
    Rng late_rng;
    bool late_shapes = false;
    // what the mid-level passes delete or must keep, from a stream of its own
    // (fuzz_gen_dead.cpp); only in GenerateDeadProgram's programs
    Rng dead_rng;
    bool dead_shapes = false;
    // what constant propagation and loop-invariant code motion change or must keep, from a
    // stream of its own (fuzz_gen_motion.cpp); only in GenerateMotionProgram's programs
    Rng motion_rng;
    bool motion_shapes = false;
    // what common subexpressions, facts from conditions and compile-time evaluation change or
    // must keep, from a stream of its own (fuzz_gen_common.cpp); only in
    // GenerateCommonProgram's programs
    Rng common_rng;
    bool common_shapes = false;
    // what loop collapse, value ranges and unread arguments change or must keep, from a
    // stream of its own (fuzz_gen_collapse.cpp); only in GenerateCollapseProgram's programs
    Rng collapse_rng;
    bool collapse_shapes = false;
    // what variable ranges, sets' ranges, elements no store reaches and slice 5's identities
    // change or must keep, from a stream of its own (fuzz_gen_values.cpp); only in
    // GenerateValuesProgram's programs. The constants vk is called with nearly always, and two
    // indexes of vt that are stored
    Rng values_rng;
    bool values_shapes = false;
    std::string values_a;
    std::string values_b;
    std::string values_m;
    bool values_vr_read = false; // whether ValuesCall reads vr's value
    int values_stored[2] = {0, 0};
    // calls through function pointers, from a stream of their own (fuzz_gen_fnptr.cpp); only
    // in GenerateFnPtrProgram's programs. fnptr_targets: writing the targets' bodies, where
    // no shape calls through a pointer (it could reach the target itself)
    Rng fnptr_rng;
    bool fnptr_shapes = false;
    bool fnptr_targets = false;
    // an interrupt handler of its own, from a stream of its own (fuzz_gen_irq.cpp); only in
    // GenerateIrqProgram's programs
    Rng irq_rng;
    bool irq_shapes = false;
    // calls inlining copies and divisions the ranges pass works out, from a stream of their
    // own (fuzz_gen_inline.cpp); only in GenerateInlineProgram's programs
    Rng inline_rng;
    bool inline_shapes = false;
    // what the code size budget turns down or keeps, and folds around calls and constant
    // indexes, from a stream of its own (fuzz_gen_size.cpp); only in GenerateSizeProgram's
    // programs
    Rng size_rng;
    bool size_shapes = false;
    // where inlined copies' locals get bytes (frame_share.hpp), from a stream of its own
    // (fuzz_gen_frames.cpp); only in GenerateFramesProgram's programs. frames_once: the
    // helper called in one place is called
    Rng frames_rng;
    bool frames_shapes = false;
    bool frames_once = false;
    // I/O through bases in globals and inlined copies handing their parameters on, from a
    // stream of its own (fuzz_gen_io.cpp); only in GenerateIoProgram's programs
    Rng io_rng;
    bool io_shapes = false;
    // const data: tables of structs with arrays, strings and addresses in them, from a stream
    // of its own (fuzz_gen_data.cpp); only in GenerateDataProgram's programs. What the reads
    // must stay inside: the foes' count, the shortest strings of each kind
    Rng data_rng;
    bool data_shapes = false;
    int data_foes = 2;
    int data_dm0_len = 1;
    int data_move_len[4] = {1, 1, 1, 1};
    int data_moves_min = 1;
    int data_name_min = 1;
    int data_names_min = 0;
    int data_foe_moves_min = 1;
    // functions in separate stretches of memory and tail calls between them, from a stream of
    // its own (fuzz_gen_stretch.cpp); only in GenerateStretchProgram's programs
    Rng stretch_rng;
    bool stretch_shapes = false;
    // Hoare's partition loops, from a stream of their own (fuzz_gen_partition.cpp); only in
    // GeneratePartitionProgram's programs
    Rng partition_rng;
    bool partition_shapes = false;
    std::vector<VarInfo> globals;
    std::vector<ArrayInfo> arrays;
    std::vector<FnInfo> fns;
    std::size_t current = 0; // the function being written; calls go to earlier ones
    std::vector<std::vector<VarInfo>> scopes;
    std::vector<PtrInfo> ptrs; // pointer locals in scope (whole function)
    int names = 0;
    int loops = 0;
    // a u16 loop variable known to stay below 512, and a pointer into wb, inside its loop
    std::string wide;
    std::string wide_ptr;
    // the innermost u8 for counter, which byte indices like to use
    std::string counter;
    bool in_while = false;
    int budget = 0;
    // sm, an array of small values (0 to 3), but now and then a program stores a big one
    bool sm_big = false;
    // race mode (GenerateRaceProgram): nothing a C program cannot say the same way (no I/O),
    // the last function is t.run, and a main and a checksum of the globals follow it
    bool race = false;
    std::string entry = "main";
    // race mode: every global's start value, set by t.init so no compiler sees it
    std::string race_init;

    std::string Fresh(const char* prefix);
    std::string Literal(T t);
    // variables of this type in scope: locals, params, globals
    std::vector<const VarInfo*> VarsOf(T t, bool writable);
    std::vector<const ArrayInfo*> ArraysOf(T t, bool writable);
    // an index that stays inside the array
    std::string Index(const ArrayInfo& a);
    std::string Var(T t);
    std::string Leaf(T t);
    // a field of type t: of an element of sa, or of gs
    std::string StructField(T t);
    // never a bare literal: those need a type from their context
    std::string NonLit(T t, int depth);
    std::string Count();
    std::string Call(T t, int depth);
    std::string CallText(const FnInfo& fn, int depth);
    const PtrInfo* Pointer(T t);
    // a count for (+ p count): any byte for a pointer with room, else 0 to 3
    std::string PointerCount(const PtrInfo& p, int depth);
    // a u8 made of a variable or counter and a constant: x, (+ x k), (- x k), (+ k x)
    std::string ByteForm();
    // a u16 count below 256: a widened byte form or value, an index let, a constant
    std::string ByteIndex(int depth);
    std::string PointerRead(T t, int depth);
    // a constant of type t from the side stream: small, an edge, or any
    std::string SideLiteral(T t);
    // x with constants chained onto it, which constant folding combines
    std::string ConstChain(T t, int depth);
    // p plus counts in a chain that stays where (+ p count) may go: (+ (+ p 1) 2)
    std::string ChainedPointer(const PtrInfo& p, int depth);
    // (op (at a i) (at b j)) on u8 tables
    std::string TwoReads(int depth);
    std::string IntExpr(T t, int depth);
    // a constant divisor: small odd and even ones, 128 and up, a few powers of two
    std::string Divisor(T t);
    // a sum of three elements of sm and a byte
    std::string SmallSum(int depth);
    // 16-bit arithmetic on widened, masked or shifted bytes: shifts, products and adds
    // with bytes known to be 0, like a hi-res row address
    std::string WideByte(int depth);
    std::string BoolExpr(int depth);
    std::string Expr(T t, int depth);
    std::string IoAddress();
    T AnyType();
    // ---- statements
    std::string Place(T& t);
    // pointers declared in scopes that ended
    void DropPointers();
    std::string Body(int depth, int count);
    // an if arm (clause false) or a match clause body whose value is dropped
    std::string Arm(int depth, int count, bool clause);
    std::string LoopBody(int depth, bool is_while);
    std::string For(int depth);
    std::string While(int depth);
    std::string Match(int depth);
    std::string PointerLet();
    // (let ix u16 (as u16 (+ x k))): a local that only names a byte index
    std::string IndexLet();
    std::string RoomPointerLet();
    // a load or store through a pointer with room, most often by the loop counter
    std::string RoomStmt();

    std::string Store();
    // a place of type t, or "" if there is none
    std::string PlaceOf(T t);
    // a / and a % of the same operands, now and then with a statement between
    std::string DivPair();
    // a store of a small value into sm, directly or through a pointer
    std::string SmallStore();
    // lets at the top of a u8 loop's body that move by a constant each iteration (a row of
    // wb through rowp, a pointer plus the counter times a stride, a u16 product), each
    // read twice; minus_one: the counter starts at 1 or more; small: it stays below 12
    std::string StrideLets(const std::string& loop_var, bool minus_one, bool small);
    std::string WideStmt();
    // ---- loops (fuzz_gen_loops.cpp)
    std::string ByteTable();
    // a while over u8 locals: stepped, set in arms, compared, read as indices
    std::string ByteWhile(int depth);
    std::string BoundWhile(int depth);
    std::string StepWhile(int depth);
    std::string ScanWhile();
    // a while over a u16 local stepped by constants or a variable, read as an index
    std::string WordWhile(int depth);
    // a u16 local stepped once a pass of a u8 for, read as an index
    std::string SteppedFor(int depth);
    // a u16 for whose body has a while of its own, reading arrays and addresses by the counter
    std::string WordFor(int depth);
    // a CRC's bit steps on a u8 or u16 local, and masks of its bits tested
    std::string BitSteps(int depth);
    std::string LoopShape(int depth);
    // Hoare's partition: inner whiles move two u16 indexes, then stores through them
    // (fuzz_gen_partition.cpp)
    std::string PartitionLoop(int depth);
    // ---- loops entered by a jump (fuzz_gen_entries.cpp)
    // a test that can never hold, on a variable in scope
    std::string NeverTrue();
    // a statement whose value goes round a loop: into the u8 g, or at wb's index k
    std::string EntryCarried(int depth, const std::string& g, const std::string& k);
    // the loop's first statement: an if on a test that never holds, as a statement or value
    std::string EntryFirst(int depth, const std::string& g, const std::string& k);
    // a loop whose body starts with an if on a test that never holds
    std::string EntryLoop(int depth);
    // ---- waits and strings (fuzz_gen_late.cpp)
    // a constant or variable (wait_cycles n)
    std::string LateWait();
    // a byte of a string literal
    std::string StringRead(int depth);
    // ---- dead work and work that only looks dead (fuzz_gen_dead.cpp)
    T DeadInt();
    std::string DeadTarget(T t);
    // a counting while over a fresh counter, ending in ways the passes can follow or not
    std::string CountingWhile(int depth);
    std::string DeadLoopItem(int depth);
    std::string DeadFor(int depth);
    // a dead let, store, statement or loop, or a loop whose result is read after it
    std::string DeadShape(int depth);
    // ---- loop-invariant work and constants (fuzz_gen_motion.cpp)
    T MotionInt();
    std::string MotionOperand(T t, bool literal);
    std::string MotionWork(T t, const std::string& a, const std::string& b);
    std::string MotionItem(T t, const std::string& work, const std::string& count_var, int depth);
    // a loop whose body works out what it (most of the time) never changes the inputs of
    std::string InvariantLoop(int depth);
    // a let of a constant, things that may change it, reads of it
    std::string ConstLet(int depth);
    std::string ConstGlobal();
    std::string SameOperands();
    std::string MotionShape(int depth);
    // ---- values worked out twice, facts from conditions, calls at compile time
    // (fuzz_gen_common.cpp)
    // ---- calls to copy and divisions to work out (fuzz_gen_inline.cpp)
    std::string InlineHelpers();
    T InlineInt();
    std::string InlineDivision();
    std::string InlineElements();
    std::string InlineFields();
    std::string InlineShape(int depth);
    // ---- the size budget, folds around calls, constant indexes (fuzz_gen_size.cpp)
    std::string SizeHelpers();
    std::string SizeCall(std::string& lets);
    std::string SizeAbsorbed();
    std::string SizeIndex();
    std::string SizeUnroll();
    std::string SizeShape(int depth);
    // ---- where inlined copies' locals get bytes (fuzz_gen_frames.cpp)
    std::string FramesHelpers();
    std::string FramesShape(int depth);
    // ---- I/O bases, parameters handed on (fuzz_gen_io.cpp)
    std::string IoHelpers();
    std::string IoBaseValue();
    std::string IoBased();
    std::string IoShape(int depth);
    std::string StretchWork(const std::string& k);
    std::string StretchInput();
    std::string StretchBig(int n);
    std::string StretchCall(int n, const std::string& arg);
    std::string StretchTail(int t, int n);
    std::string StretchHelpers();
    std::string StretchShape(int depth);
    // ---- const data (fuzz_gen_data.cpp)
    std::string DataString(int& len);
    std::string DataByte();
    std::string DataMove(bool copies, int& min_len);
    // DMv, DFoe, dstep0-1, dglob, dacc, DM0, DMOVES, DFOES, DNAMES, DPTR, DGP and dcur,
    // before the generated functions
    std::string DataHelpers();
    std::string DataFoe();
    std::string DataIndex(int n);
    std::string DataMoveIndex();
    std::string DataValue();
    std::string DataShape(int depth);
    // the pure helpers common shapes call, after peek2 and poke2
    std::string CommonHelpers();
    T CommonInt();
    std::string CommonVar(T t);
    std::string CommonWork(T t, const std::string& a, const std::string& b);
    std::string CommonBetween(T t, const std::string& a, int depth);
    std::string CommonTwice(int depth);
    std::string DivApart(int depth);
    std::string CondFact(int depth);
    std::string ConstCall();
    std::string CommonShape(int depth);
    // ---- loops that collapse, compares ranges decide (fuzz_gen_collapse.cpp)
    T CollapseInt();
    std::string CollapseVar(T t);
    std::string CollapseItem(const std::string& count, T count_type, bool is_for, int depth);
    std::string CollapseLoop(int depth);
    std::string RangeTest();
    std::string CounterMatch();
    std::string CollapseShape(int depth);
    // ---- variable ranges, sets' ranges, unstored elements (fuzz_gen_values.cpp)
    T ValuesInt();
    std::string ValuesVar(T t);
    std::string EdgeLiteral(T t);
    // vg, vt, vq and the helpers vk and vm, before the generated functions
    std::string ValuesHelpers();
    std::string ValuesCall();
    std::string StepLoop(int depth);
    std::string SetFacts(int depth);
    std::string ElementShape();
    std::string ContinueLoop(int depth);
    std::string Complements();
    std::string SwitchLoop(int depth);
    std::string ValuesShape(int depth);
    // ---- calls through function pointers (fuzz_gen_fnptr.cpp)
    std::string FnPtrVar(T t);
    std::string FnPtrBody(T ret, bool pure);
    // fq, the targets fa0-2 (fn (u8 u16) u8), fb0-2 (fn (u8) void), fc0-2 (fn () u16), the
    // table FTA, fhb, fgc, fos, finit, fapply and fpick, before the generated functions
    std::string FnPtrHelpers();
    std::string FnPtrIndex();
    // a value of type t (u8 or u16) from a call through a pointer, or a direct call of a
    // target; the lets it needs go into lets
    std::string FnPtrValue(T t, std::string& lets);
    std::string FnPtrShape(int depth);
    // ---- an interrupt handler (fuzz_gen_irq.cpp)
    std::string IrqValue(bool two, int depth);
    std::string IrqStmt(int depth);
    // hq0-2, hw0-1, hs, ha, the helper hf and the handler on_irq, before the generated
    // functions
    std::string IrqHelpers();
    std::string IrqMask(int depth);
    std::string Stmt(int depth);
    std::string Function(std::size_t index);
    std::string Program();
    // ---- race mode (fuzz_gen_race.cpp)
    // a scalar global's start value (" 5", or "" for none) as a set in t.init; returns ""
    std::string RaceStart(const std::string& name, T t, const std::string& init);
    // an array's " (make (array t n) v...)" as sets in t.init; returns ""
    std::string RaceArrayStart(const std::string& name, T elem, const std::string& init);
    // gs's fields, set in t.init; returns ""
    std::string RaceStructStart();
    // a store of a byte into a variable or element, where a program outside race mode writes
    // to or reads from I/O
    std::string RaceStore();
    // rv, chk, t.init, the checksum function and main, after the generated functions
    std::string RaceTail();
};

} // namespace tisple_tests::fuzz
