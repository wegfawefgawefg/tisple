// with_aux in the checker and the interpreter: the form, what code that runs with the switches
// on may not do (each rule's error), every exit turning them off, and the interpreter's aux
// memory, which pointer accesses in a region reach as the switches say

#include "check/aux_mode.hpp"
#include "test_check.hpp"

namespace tisple_tests {

namespace {

const char* kPointers = "(let p (ptr u8) (as (ptr u8) 0x2000))\n";

void FormTests(TestResults& results) {
    ExpectError(results, "(fn f () void (with_aux true))",
                "t.tsp:1:15: with_aux takes whether it reads and whether it writes aux memory, "
                "then its body; write (with_aux true false body ...)");
    ExpectError(results, "(fn f ((n u8)) void (with_aux n false))",
                "t.tsp:1:31: with_aux's reads needs a bool but n is u8; write (!= n 0)");
    ExpectError(results, "(fn f () u8 (with_aux true false))",
                "t.tsp:1:13: this with_aux has no body to give a value; add one");
    ExpectError(results,
                "(fn f ((p (ptr u8))) void\n    (with_aux true false\n        (with_aux false "
                "true (store p 1))))",
                "t.tsp:3:9: with_aux inside with_aux: the inner one would turn the switches off "
                "before the outer one ends; give the outer one the switches both need");
    // a value, a runtime bool, a body of statements, an empty one
    Built ok = BuildText("(fn f ((p (ptr u8)) (aux bool)) u8\n"
                         "    (with_aux aux false (load p)))\n"
                         "(fn g ((p (ptr u8)) (aux bool)) void\n"
                         "    (with_aux false aux (store p 1) (store (+ p 1) 2))\n"
                         "    (with_aux false false))");
    CHECK(results, ok.ok);
}

void RuleTests(TestResults& results) {
    std::string g = "(let g u8 0)\n(let z u8 :address 0x00F0)\n(const T (array u8 2) (make "
                    "(array u8 2) 5 6))\n";
    ExpectError(results, g + "(fn f () void (with_aux false true (set g 1)))",
                "t.tsp:4:41: (set g ...) with RAMWRT on would write aux memory's byte at g's "
                "address: g is a global outside zero page; set it after the with_aux, or give it "
                "an :address in zero page");
    ExpectError(results, g + "(fn f () u8 (with_aux true false g))",
                "t.tsp:4:34: g is read with RAMRD on, which would read aux memory's byte at its "
                "address: it is a global outside zero page; copy it into a local before the "
                "with_aux, or give it an :address in zero page");
    // zero page globals, sets with RAMRD alone, reads with RAMWRT alone, const data, locals
    Built ok = BuildText(g + "(fn f ((a u8)) u8\n"
                             "    (let k u8 a)\n"
                             "    (with_aux true true (set z (+ z k)) (set k (at T 1)))\n"
                             "    (with_aux true false (set g (at T 0)))\n"
                             "    (with_aux false true (set z g))\n"
                             "    k)");
    CHECK(results, ok.ok);
    // the rules hold in what the region calls, and what that calls
    ExpectError(results,
                g + "(fn put ((v u8)) void (set g v))\n(fn mid ((v u8)) void (put v))\n"
                    "(fn f () void (with_aux false true (mid 1)))",
                "t.tsp:4:28: (set g ...) with RAMWRT on would write aux memory's byte at g's "
                "address: g is a global outside zero page; set it after the with_aux, or give it "
                "an :address in zero page (t.put runs inside a with_aux, called from t.mid)");
    ExpectError(results,
                "(fn step ((p (ptr u8))) void (with_aux true false (load p)))\n"
                "(fn f ((p (ptr u8))) void (with_aux false true (step p)))",
                "t.tsp:1:30: with_aux inside with_aux: the inner one would turn the switches off "
                "before the outer one ends; give the outer one the switches both need (t.step "
                "runs inside a with_aux, called from t.f)");
    ExpectError(results,
                "(fn walk ((i u8)) void)\n(let f (fn (u8) void) walk)\n"
                "(fn main () void (let h (fn (u8) void) f) (with_aux true true (h 1)))",
                "t.tsp:3:63: a call through a function pointer with the aux switches on: its "
                "dispatcher stores the address into its own code; call the function by name");
    ExpectError(results, "(fn f ((a u16)) void (with_aux true false (io_write u8 a 0)))",
                "t.tsp:1:43: an address worked out at run time with the aux switches on: the code "
                "stores it into its own instruction, which aux memory's copy does not see; use a "
                "constant address, or do it outside the with_aux");
    ExpectError(results, "(fn f ((a u16)) void (with_aux false true (jsr a)))",
                "t.tsp:1:43: an address worked out at run time with the aux switches on: the code "
                "stores it into its own instruction, which aux memory's copy does not see; use a "
                "constant address, or do it outside the with_aux");
    // constant I/O and machine code are fine; so is a function that runs both inside and out
    Built fine = BuildText(g + "(fn twice ((v u8)) u8 (+ v v))\n"
                               "(fn f ((p (ptr u8))) u8\n"
                               "    (let a u8 (twice 3))\n"
                               "    (with_aux true true (io_write u8 0xC030 0) (jsr 0xFBDD)\n"
                               "        (store p (twice (load p))))\n"
                               "    a)");
    CHECK(results, fine.ok);
}

// where the switches may be on: inside, in what is called inside, and not after any exit
void ModeTests(TestResults& results) {
    Built b = BuildText(std::string(kPointers) + "(fn leaf ((q (ptr u8))) u8 (load q))\n"
                                                 "(fn outside ((q (ptr u8))) u8 (leaf q))\n"
                                                 "(fn f ((q (ptr u8)) (c bool)) u8\n"
                                                 "    (with_aux false c (store q (leaf q)))\n"
                                                 "    (outside q))");
    CHECK(results, b.ok);
    if (!b.ok) {
        return;
    }
    tisple::AuxModes modes = tisple::FindAuxModes(b.program);
    std::int32_t leaf = tisple::FindFunction(b.program, "t.leaf");
    std::int32_t outside = tisple::FindFunction(b.program, "t.outside");
    std::int32_t f = tisple::FindFunction(b.program, "t.f");
    CHECK(results, modes.any && modes.entry[static_cast<std::size_t>(leaf)] == tisple::kAuxWrites);
    CHECK(results, modes.entry[static_cast<std::size_t>(outside)] == 0);
    CHECK(results, modes.owns[static_cast<std::size_t>(f)] == tisple::kAuxWrites);
    CHECK(results, tisple::AuxFunction(modes, leaf) && !tisple::AuxMirrored(modes, leaf));
    CHECK(results, !tisple::AuxFunction(modes, outside));
}

// aux memory in the interpreter: pointer accesses of $0200-$BFFF in a region go where the
// switches say; variables, zero page and the stack stay main memory's; every exit turns both
// off
void InterpTests(TestResults& results) {
    Built b = BuildText("(let buf (array u8 4) (make (array u8 4) 1 2 3 4) :in aux)\n"
                        "(let mine (array u8 4))\n"
                        "(let zp u8 7 :address 0x0080)\n"
                        "(fn get ((i u8)) u8 (with_aux true false (load (addr_of (at buf i)))))\n"
                        "(fn put ((i u8) (v u8)) void\n"
                        "    (with_aux false true (store (addr_of (at buf i)) v)))\n"
                        "(fn swap ((i u8)) u8\n"
                        "    (with_aux true true\n"
                        "        (let was u8 (load (addr_of (at buf i))))\n"
                        "        (store (addr_of (at mine i)) was)\n"
                        "        (store (addr_of zp) (+ (load (addr_of zp)) 1))\n"
                        "        was))\n"
                        "(fn find ((v u8)) u8\n"
                        "    (with_aux true false\n"
                        "        (for i u8 0 4\n"
                        "            (if (= (load (addr_of (at buf i))) v)\n"
                        "                (return i))))\n"
                        "    255)\n"
                        "(fn count ((v u8)) u8\n"
                        "    (let n u8 0)\n"
                        "    (for i u8 0 4\n"
                        "        (with_aux true false\n"
                        "            (if (= (load (addr_of (at buf i))) v)\n"
                        "                (break))\n"
                        "            (set n (+ n 1))))\n"
                        "    n)\n"
                        "(fn mine_at ((i u8)) u8 (at mine i))");
    CHECK(results, b.ok);
    if (!b.ok) {
        return;
    }
    tisple::Interp in;
    StartInterp(in, b);
    CHECK(results, CallOk(results, in, "t.get", {2}) == 3);
    CallOk(results, in, "t.put", {1, 9});
    CHECK(results, CallOk(results, in, "t.get", {1}) == 9);
    CHECK(results, !in.aux_reads && !in.aux_writes);
    // RAMRD and RAMWRT both: the aux byte is read, the store to mine goes to aux memory too, the
    // zero page byte is main memory's
    CHECK(results, CallOk(results, in, "t.swap", {1}) == 9);
    CHECK(results, CallOk(results, in, "t.mine_at", {1}) == 0);
    std::uint16_t mine =
        in.layout->vars[static_cast<std::size_t>(tisple::FindVar(b.program, "t.mine"))].address;
    CHECK(results, in.aux[mine + 1u] == 9 && in.memory[0x80] == 8);
    // an early return and a break that leave the region turn the switches off
    CHECK(results, CallOk(results, in, "t.find", {4}) == 3 && !in.aux_reads);
    CHECK(results, CallOk(results, in, "t.find", {77}) == 255);
    CHECK(results, CallOk(results, in, "t.count", {3}) == 2 && !in.aux_reads);
}

} // namespace

void RunCheckAuxTests(TestResults& results) {
    FormTests(results);
    RuleTests(results);
    ModeTests(results);
    InterpTests(results);
}

} // namespace tisple_tests
