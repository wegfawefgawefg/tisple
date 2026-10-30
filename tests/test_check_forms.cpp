#include "test_check.hpp"

namespace tisple_tests {

namespace {

void ControlTests(TestResults& results) {
    ExpectError(results, "(fn f ((x u8)) void (if x (return)))",
                "t.tsp:1:25: the if condition needs a bool but x is u8; write (!= x 0)");
    ExpectError(results, "(fn f ((c bool) (a u8) (b u16)) u16 (if c a b))",
                "t.tsp:1:45: if arms give u8 and u16; make them the same type, as in (as u16 a)");
    ExpectError(results, "(fn f ((c bool)) bool (= (if c 1 2) 3))",
                "t.tsp:1:32: 1 has no type here; a literal takes the type of what it meets, so "
                "use it next to a typed value or declare one, as in (let n u8 1)");
    // only a used value needs arms of one type
    Built statement = BuildText("(let x u16 0)\n(fn g () u8 1)\n"
                                "(fn f ((c bool) (k u8)) void\n"
                                "    (if c (g) (set x 1))\n"
                                "    (match k (0 (g)) (1 x) (_))\n"
                                "    (if c (do (g) x) (g)))");
    CHECK(results, statement.ok);
    ExpectError(results, "(fn f ((c bool)) void (if c 1 2))",
                "t.tsp:1:29: 1 on its own does nothing; remove it");
    ExpectError(results, "(fn f ((c bool)) void (if c (do) (do) (do)))",
                "t.tsp:1:23: if takes a condition and one or two arms; group forms with (do ...)");
    ExpectError(results, "(fn f () void (break))", "t.tsp:1:15: break is only inside while or for");
    ExpectError(results, "(fn f () void (while true (continue 1)))",
                "t.tsp:1:27: continue takes nothing; write (continue)");
    ExpectError(results, "(fn f () void (while 1))",
                "t.tsp:1:22: 1 is not a bool; write true or false");
}

void ReturnTests(TestResults& results) {
    ExpectError(results, "(fn f () u8 (let x u8 0))",
                "t.tsp:1:13: f returns u8 but its last form gives no value; end the body with a "
                "u8 value or (return v)");
    ExpectError(results, "(fn f () u8)",
                "t.tsp:1:1: f returns u8 but its last form gives no value; end the body with a "
                "u8 value or (return v)");
    ExpectError(results, "(fn f () void (return 1))",
                "t.tsp:1:15: f returns void, so it gives no value; write (return) or give f a "
                "return type");
    ExpectError(results, "(fn f () u8 (return))", "t.tsp:1:13: f returns u8; write (return value)");
    ExpectError(results, "(fn f ((a u16)) u8 a)",
                "t.tsp:1:20: the value of f needs a u8 but a is u16; write (as u8 a)");
    ExpectError(results, "(fn f ((a u16)) u8 (return a))",
                "t.tsp:1:28: the return value of f needs a u8 but a is u16; write (as u8 a)");
    // a value after the last form is fine when every path returns
    Built ok = BuildText("(fn f ((c bool)) u8 (if c (return 1) (return 2)))");
    CHECK(results, ok.ok);
}

void MatchTests(TestResults& results) {
    const std::string a = "(enum A (one two three))\n";
    ExpectError(results, a + "(fn f ((a A)) void (match a (A.one)))",
                "t.tsp:2:20: match on A does not cover A.two, A.three; add (A.two ...) (A.three "
                "...) or a (_ ...) clause");
    ExpectError(results, "(fn f ((x u8)) void (match x (0)))",
                "t.tsp:1:21: a match on u8 needs a (_ ...) clause last, for the values no "
                "pattern names");
    ExpectError(results, "(fn f ((x u8)) void (match x (_) (0)))",
                "t.tsp:1:34: _ matches everything, so this clause never runs; move (_ ...) last");
    ExpectError(results, "(fn f ((x u8)) void (match x (0) (0) (_)))",
                "t.tsp:1:35: 0 is matched twice; remove one clause");
    ExpectError(results, "(enum A (one))\n(fn f ((a A)) void (match a (A.one) (_)))",
                "t.tsp:2:20: every member of A is already matched; remove the (_ ...) clause");
    ExpectError(results, a + "(fn f ((a A)) void (match a (one) (_)))",
                "t.tsp:2:30: enum members are written qualified; write A.one");
    ExpectError(results, "(fn f ((b bool)) void (match b (true) (_)))",
                "t.tsp:1:23: match on a bool is an if; write (if b a b)");
    ExpectError(results, "(fn f ((x u8)) void (match x (300) (_)))",
                "t.tsp:1:31: 300 does not fit in u8 (0 to 255)");
    ExpectError(
        results, "(fn f ((x u8)) u16 (match x (0 x) (_ (as u16 x))))",
        "t.tsp:1:38: match arms give u8 and u16; make them the same type, as in (as u16 x)");
    ExpectError(results, "(enum A ((x 1) (y 1)))",
                "t.tsp:1:17: A.y and A.x both have value 1; give each member its own value");
}

void LoopTests(TestResults& results) {
    ExpectError(results, "(fn f () void (for i 0 10))",
                "t.tsp:1:15: for always states the type; write (for i u8 0 10 ...)");
    ExpectError(results, "(fn f () void (for i 0 1000))",
                "t.tsp:1:15: for always states the type; write (for i u16 0 1000 ...)");
    ExpectError(results, "(fn f ((n u16)) void (for i 0 n))",
                "t.tsp:1:22: for always states the type; write (for i u16 0 n ...)");
    ExpectError(results, "(fn f () void (for i u8 0 10 (set i 3)))",
                "t.tsp:1:35: the loop variable i is read-only; copy it into a let to change it");
    ExpectError(results, "(fn f ((n u16)) void (for i u8 0 n))",
                "t.tsp:1:34: the end of the loop needs a u8 but n is u16; write (as u8 n)");
    ExpectError(results, "(fn f () void (for i u8 0 257))",
                "t.tsp:1:27: 257 does not fit in u8, the loop's type; write (for i u16 0 257 ...)");
    ExpectError(
        results, "(fn f () void (for i i8 -1 200))",
        "t.tsp:1:28: 200 does not fit in i8, the loop's type; write (for i i16 -1 200 ...)");
    // a one-armed if is a statement: as a value it is one error, not one about its arm too
    ExpectError(results, "(fn f ((x u8)) u8 (let k u8 (if (= x 0) 1)) k)",
                "t.tsp:1:29: an if without an else gives no value; write (if (= x 0) 1 other)");
    CHECK(results, ErrorText(BuildText("(fn f ((x u8)) u8 (let k u8 (if (= x 0) 1)) k)"))
                           .find("does nothing") == std::string::npos);
    ExpectError(results, "(fn f ((b bool)) void (for i b true))",
                "t.tsp:1:23: for counts with an integer type, not bool");
    // (for i u8 0 256 ...) covers every byte
    CHECK(results, BuildText("(fn f () void (for i u8 0 256))").ok);
    CHECK(results, BuildText("(fn f () void (for i i8 -128 128))").ok);
}

void PlaceTests(TestResults& results) {
    const std::string t = "(let t (array u8 10))\n";
    ExpectError(
        results, t + "(fn f ((i u16)) u8 (at t i))",
        "t.tsp:2:26: the index into t (10 elements) needs a u8 but i is u16; write (as u8 i)");
    ExpectError(results, t + "(fn f () u8 (at t 10))",
                "t.tsp:2:19: index 10 is past the end of t (10 elements)");
    ExpectError(results, "(let big (array u8 300))\n(fn f ((i u8)) u8 (at big i))",
                "t.tsp:2:27: the index into big (300 elements) needs a u16 but i is u8; write (as "
                "u16 i)");
    ExpectError(results, "(fn f ((p (ptr u8))) u8 (at p 0))",
                "t.tsp:1:25: at indexes arrays; through a pointer write (load (+ p 0))");
    ExpectError(results, "(fn f ((a u16)) u8 (load a))",
                "t.tsp:1:26: load goes through a pointer; a is u16; write (as (ptr u8) a)");
    ExpectError(results, "(const K u8 1)\n(fn f () (ptr u8) (addr_of K))",
                "t.tsp:2:28: K is a number with no address; give it storage with a top-level "
                "let, or make it a const table");
    ExpectError(results, "(const K u8 1)\n(fn f () void (set K 2))",
                "t.tsp:2:20: K is a constant; it cannot be set");
    ExpectError(results,
                "(const T (array u8 2) (make (array u8 2) 1 2))\n(fn f () void (set (at T 0) 2))",
                "t.tsp:2:20: (at T 0) is part of a constant; it cannot be set");
    ExpectError(results, "(struct P ((x u8)))\n(fn f ((p (ptr P))) void (set (load p x) 1))",
                "t.tsp:2:31: set does not write through pointers; write (store p x 1)");
    ExpectError(results, "(fn f ((x u8)) void (set (+ x 1) 2))",
                "t.tsp:1:26: set needs a variable, s.x or (at ...); (+ x 1) is not one");
    ExpectError(results, t + "(let u (array u8 10))\n(fn f () void (set t u))",
                "t.tsp:3:15: arrays are copied element by element; use a for loop");
    ExpectError(results, "(fn f ((p (ptr u8))) void (store p 300))",
                "t.tsp:1:36: 300 does not fit in u8 (0 to 255)");
}

void DeclarationTests(TestResults& results) {
    ExpectError(results, "(fn g () u8 1)\n(let x u8 (g))",
                "t.tsp:2:11: a top-level let starts with a constant value; set x at the start of "
                "main instead");
    ExpectError(results, "(const T (array u8 3) (make (array u8 3) 1 2))",
                "t.tsp:1:23: T is (array u8 3) but its value has 2 values; give exactly 3");
    ExpectError(results, "(let x)", "t.tsp:1:1: let x needs a type; write (let x u8 0)");
    ExpectError(results, "(const A u8 B)\n(const B u8 A)",
                "t.tsp:1:1: constant A depends on itself; give it a value that does not use A");
    ExpectError(results, "(+ 1 2)",
                "t.tsp:1:1: only import, const, let, struct, enum, fn and macro go at top "
                "level; put (+ ...) inside a fn");
    ExpectError(results, "(fn f () void (let x u8 0 :align 2))",
                "t.tsp:1:27: placement options (:address, :align, :boundary, :in, :reserve) are "
                "for top-level lets; move x out of the function");
    ExpectError(results, "(let x u8 0 :in zero_page)",
                "t.tsp:1:17: :in takes aux, the one named region; write :in aux");
    ExpectError(results, "(let x u8 0 :in)",
                "t.tsp:1:13: :in takes aux, the one named region; write :in aux");
    ExpectError(results, "(let x u8 0 :in aux :in aux)",
                "t.tsp:1:21: :in is given twice; keep one");
    ExpectError(results, "(let x u8 0 :nearby 1)",
                "t.tsp:1:13: :nearby is not a let option; use :address, :align, :boundary, :in "
                "aux or :reserve");
    // :reserve: room at a fixed address the program reaches with pointers
    ExpectError(results, "(let page (array u8 8192) :reserve)",
                "t.tsp:1:1: :reserve keeps memory at a fixed address; give it one: (let page "
                "(array u8 8192) :address 0x2000 :reserve)");
    ExpectError(results, "(let page u8 7 :address 0x2000 :reserve)",
                "t.tsp:1:1: page is :reserve, memory the program reaches by address, so it has no "
                "value; drop the value or :reserve");
    ExpectError(results,
                "(let page (array u8 8192) :address 0x2000 :reserve)\n"
                "(fn f () u8 (at page 3))",
                "t.tsp:2:17: page is :reserve memory, not a variable; reach it with a pointer, as "
                "in (as (ptr u8) 0x2000)");
    ExpectError(results, "(const T (array u8 2) (make (array u8 2) 1 2) :reserve)",
                "t.tsp:1:47: :reserve is for top-level lets; make T a let");
    // :in aux: reached only through addr_of and lib/auxmem
    ExpectError(results, "(const T (array u8 2) (make (array u8 2) 1 2) :in aux)",
                "t.tsp:1:47: :in aux is for top-level lets; make T a let");
    ExpectError(results, "(let doc (array u8 100) :in aux)\n(fn f () u8 (at doc 3))",
                "t.tsp:2:17: doc is in aux memory, which code reaches only through "
                "lib/auxmem; copy it with (auxmem.read (addr_of buffer) (addr_of doc) count)");
    ExpectError(results, "(let n u8 :in aux)\n(fn f () void (set n 3))",
                "t.tsp:2:20: n is in aux memory, which code reaches only through lib/auxmem; "
                "copy it with (auxmem.read (addr_of buffer) (addr_of n) count)");
    ExpectError(results, "(let doc (array u8 16) :address 0x0100 :in aux)",
                "t.tsp:1:1: doc is in aux memory at an :address outside $0200-$BFFF, which "
                "lib/auxmem cannot reach; move it");
    CHECK(results, BuildText("(let doc (array u8 100) :in aux)\n"
                             "(fn f () u16 (as u16 (addr_of (at doc 7))))")
                       .ok);
    CHECK(results, BuildText("(let page (array u8 8192) :address 0x2000 :reserve)\n"
                             "(fn f () u8 (load (as (ptr u8) 0x2005)))")
                       .ok);
    ExpectError(results, "(let x u8 0 :align 0)",
                "t.tsp:1:20: :align 0 means nothing; use 1 or more, like :align 256");
    ExpectError(results, "(let t (array u8 3) 1)",
                "t.tsp:1:21: t is (array u8 3), so its value is (make (array u8 3) v0 v1 ...)");
    ExpectError(results, "(let t (array u8 3) (make (array u8 4) 1 2 3 4))",
                "t.tsp:1:27: the value is (array u8 4) but t is (array u8 3); write (make (array "
                "u8 3) v0 v1 ...)");
    ExpectError(results, "(fn f () void (let t (array u8 2) (make (array u8 2) 1 2)))",
                "t.tsp:1:35: arrays start zeroed and are copied element by element; fill t with a "
                "for loop");
    ExpectError(results, "(fn f () u8 (at (make (array u8 2) 1 2) 0))",
                "t.tsp:1:17: an array value only starts a const or a top-level let; fill this "
                "array element by element with a for loop");
    ExpectError(results, "(fn f () u8 (make u8 1))",
                "t.tsp:1:13: make builds structs and arrays; a u8 is written as the value itself");
    ExpectError(results, "(let t (array u8 0))",
                "t.tsp:1:18: an array has 1 to 65535 elements, not 0");
    ExpectError(results, "(fn f () void (const K u8 1))",
                "t.tsp:1:15: const goes at top level; use let here");
    ExpectError(results, "(const D u8 (/ 1 0))", "t.tsp:1:13: division by zero in a constant");
    // constants fold in any order, through other modules
    Built folded = BuildText("(const B u8 (+ A 1))\n(const A u8 (* 2 3))\n(let t (array u8 B))");
    CHECK(results, folded.ok);
}

// the spellings before the 2026-10-26 syntax batch each say what to write now
void OldSpellingTests(TestResults& results) {
    ExpectError(results, "(fn f (args (x u8)) (returns u8) (+ x 1))",
                "t.tsp:1:1: parameters are always a list now: write (fn f ((x u8)) u8 ...)");
    ExpectError(results, "(fn f (g))\n(fn g () void)",
                "t.tsp:1:1: parameters are always a list now: write (fn f () void ...)");
    ExpectError(results, "(fn f ((x u8)) (returns u8) x)",
                "t.tsp:1:1: the return type is always the third item now: write (fn f ((x u8)) u8 "
                "...)");
    ExpectError(results, "(fn f () (g))\n(fn g () void)",
                "t.tsp:1:1: the return type comes after the parameters, void when f returns "
                "nothing; write (fn f () void ...)");
    ExpectError(results, "(macro inc (args p) `(set ,p 1))",
                "t.tsp:1:12: parameters are always a list now: write (macro inc (p) `(...))");
    ExpectError(results, "(macro m `(do))",
                "t.tsp:1:1: parameters are always a list now: write (macro m () `(...))");
    ExpectError(results, "(fn f ((x u16)) u8 (u8 x))",
                "t.tsp:1:20: a type never heads a form; to convert write (as u8 x)");
    ExpectError(results, "(struct P ((x u8)))\n(let p P (P :x 1))",
                "t.tsp:2:10: a type never heads a form; to build a P write (make P :x 1)");
    ExpectError(
        results, "(struct P (x u8) (y u8))",
        "t.tsp:1:1: fields are one list, like parameters: write (struct P ((x u8) (y u8)))");
    ExpectError(results, "(enum A one two)",
                "t.tsp:1:1: members are one list, like parameters: write (enum A (one two))");
    ExpectError(results, "(enum A (one 7))",
                "t.tsp:1:1: a member with a value is (name number) inside the list; write (enum A "
                "((one 7)))");
    ExpectError(results, "(fn f () u8 (let k (io_read u8 0xC000)) k)",
                "t.tsp:1:20: let always states the type; write (let k u8 (io_read u8 0xC000))");
    ExpectError(results, "(const T (array u8 2) 1 2)",
                "t.tsp:1:1: a value is one form; write (const T (array u8 2) (make (array u8 2) 1 "
                "2 ...))");
    ExpectError(results, "(const K (+ 1 0x100))",
                "t.tsp:1:10: (+ 1 0x100): neither side has a type; a literal takes the type of "
                "what it meets, so use a typed value on one side");
    ExpectError(results, "(struct P ((x u8)))\n(let ps (array P 2))\n(fn f () u8 (. (at ps 1) x))",
                "t.tsp:3:13: (. (at ps 1) x) is for macro templates; write (at ps 1 x)");
}

} // namespace

void RunCheckFormTests(TestResults& results) {
    ControlTests(results);
    ReturnTests(results);
    MatchTests(results);
    LoopTests(results);
    PlaceTests(results);
    DeclarationTests(results);
    OldSpellingTests(results);
}

} // namespace tisple_tests
