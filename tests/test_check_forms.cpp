#include "test_check.hpp"

namespace tisple_tests {

namespace {

void ControlTests(TestResults& results) {
    ExpectError(results, "(fn f (args (x u8)) (if x (return)))",
                "t.tsp:1:25: the if condition needs a bool but x is u8; write (!= x 0)");
    ExpectError(results, "(fn f (args (c bool) (a u8) (b u16)) (returns u16) (if c a b))",
                "t.tsp:1:60: if arms give u8 and u16; make them the same type, as in (u16 a)");
    ExpectError(results, "(fn f (args (c bool)) (if c 1 2))",
                "t.tsp:1:29: 1 has no type here; a literal takes the type of what it meets, so "
                "use it next to a typed value or declare one, as in (let n u8 1)");
    ExpectError(results, "(fn f (args (c bool)) (if c (do) (do) (do)))",
                "t.tsp:1:23: if takes a condition and one or two arms; group forms with (do ...)");
    ExpectError(results, "(fn f (break))", "t.tsp:1:7: break is only inside while or for");
    ExpectError(results, "(fn f (while true (continue 1)))",
                "t.tsp:1:19: continue takes nothing; write (continue)");
    ExpectError(results, "(fn f (while 1))", "t.tsp:1:14: 1 is not a bool; write true or false");
}

void ReturnTests(TestResults& results) {
    ExpectError(results, "(fn f (returns u8) (let x u8 0))",
                "t.tsp:1:20: f returns u8 but its last form gives no value; end the body with a "
                "u8 value or (return v)");
    ExpectError(results, "(fn f (returns u8))",
                "t.tsp:1:1: f returns u8 but its last form gives no value; end the body with a "
                "u8 value or (return v)");
    ExpectError(results, "(fn f (return 1))",
                "t.tsp:1:7: f has no (returns t), so it gives no value; write (return) or add "
                "(returns t) after its args");
    ExpectError(results, "(fn f (returns u8) (return))",
                "t.tsp:1:20: f returns u8; write (return value)");
    ExpectError(results, "(fn f (args (a u16)) (returns u8) a)",
                "t.tsp:1:35: the value of f needs a u8 but a is u16; write (u8 a)");
    ExpectError(results, "(fn f (args (a u16)) (returns u8) (return a))",
                "t.tsp:1:43: the return value of f needs a u8 but a is u16; write (u8 a)");
    // a value after the last form is fine when every path returns
    Built ok = BuildText("(fn f (args (c bool)) (returns u8) (if c (return 1) (return 2)))");
    CHECK(results, ok.ok);
}

void MatchTests(TestResults& results) {
    const std::string a = "(enum A one two three)\n";
    ExpectError(results, a + "(fn f (args (a A)) (match a (A.one (do))))",
                "t.tsp:2:20: match on A does not cover A.two, A.three; add (A.two ...) (A.three "
                "...) or a (_ ...) clause");
    ExpectError(results, "(fn f (args (x u8)) (match x (0 (do))))",
                "t.tsp:1:21: a match on u8 needs a (_ ...) clause last, for the values no "
                "pattern names");
    ExpectError(results, "(fn f (args (x u8)) (match x (_ (do)) (0 (do))))",
                "t.tsp:1:39: _ matches everything, so this clause never runs; move (_ ...) last");
    ExpectError(results, "(fn f (args (x u8)) (match x (0 (do)) (0 (do)) (_ (do))))",
                "t.tsp:1:40: 0 is matched twice; remove one clause");
    ExpectError(results, "(enum A one)\n(fn f (args (a A)) (match a (A.one (do)) (_ (do))))",
                "t.tsp:2:20: every member of A is already matched; remove the (_ ...) clause");
    ExpectError(results, a + "(fn f (args (a A)) (match a (one (do)) (_ (do))))",
                "t.tsp:2:30: enum members are written qualified; write A.one");
    ExpectError(results, "(fn f (args (b bool)) (match b (true (do)) (_ (do))))",
                "t.tsp:1:23: match on a bool is an if; write (if b a b)");
    ExpectError(results, "(fn f (args (x u8)) (match x (300 (do)) (_ (do))))",
                "t.tsp:1:31: 300 does not fit in u8 (0 to 255)");
    ExpectError(results, "(fn f (args (x u8)) (returns u16) (match x (0 x) (_ (u16 x))))",
                "t.tsp:1:53: match arms give u8 and u16; make them the same type, as in (u16 x)");
    ExpectError(results, "(enum A (x 1) (y 1))",
                "t.tsp:1:16: A.y and A.x both have value 1; give each member its own value");
}

void LoopTests(TestResults& results) {
    ExpectError(results, "(fn f (for i 0 10))",
                "t.tsp:1:7: (for i 0 10 ...): the loop variable needs a type; write (for i u8 0 "
                "10 ...)");
    ExpectError(results, "(fn f (for i 0 1000))",
                "t.tsp:1:7: (for i 0 1000 ...): the loop variable needs a type; write (for i u16 "
                "0 1000 ...)");
    ExpectError(results, "(fn f (for i u8 0 10 (set i 3)))",
                "t.tsp:1:27: the loop variable i is read-only; copy it into a let to change it");
    ExpectError(results, "(fn f (args (n u16)) (for i u8 0 n))",
                "t.tsp:1:34: the end of the loop needs a u8 but n is u16; write (u8 n)");
    ExpectError(results, "(fn f (for i u8 0 257))",
                "t.tsp:1:19: 257 does not fit in u8 (0 to 255)");
    ExpectError(results, "(fn f (args (b bool)) (for i b true))",
                "t.tsp:1:23: for counts with an integer type, not bool");
    // (for i u8 0 256 ...) covers every byte
    CHECK(results, BuildText("(fn f (for i u8 0 256))").ok);
    CHECK(results, BuildText("(fn f (for i i8 -128 128))").ok);
}

void PlaceTests(TestResults& results) {
    const std::string t = "(let t (array u8 10))\n";
    ExpectError(results, t + "(fn f (args (i u16)) (returns u8) (at t i))",
                "t.tsp:2:41: the index into t (10 elements) needs a u8 but i is u16; write (u8 i)");
    ExpectError(results, t + "(fn f (returns u8) (at t 10))",
                "t.tsp:2:26: index 10 is past the end of t (10 elements)");
    ExpectError(results, "(let big (array u8 300))\n(fn f (args (i u8)) (returns u8) (at big i))",
                "t.tsp:2:42: the index into big (300 elements) needs a u16 but i is u8; write (u16 "
                "i)");
    ExpectError(results, "(fn f (args (p (ptr u8))) (returns u8) (at p 0))",
                "t.tsp:1:40: at indexes arrays; through a pointer write (load (+ p 0))");
    ExpectError(results, "(fn f (args (a u16)) (returns u8) (load a))",
                "t.tsp:1:41: load goes through a pointer; a is u16; write (as (ptr u8) a)");
    ExpectError(results, "(const K u8 1)\n(fn f (returns (ptr u8)) (addr_of K))",
                "t.tsp:2:35: constants have no address; use a top-level let");
    ExpectError(results, "(const K u8 1)\n(fn f (set K 2))",
                "t.tsp:2:12: K is a constant; it cannot be set");
    ExpectError(results, "(const T (array u8 2) 1 2)\n(fn f (set (at T 0) 2))",
                "t.tsp:2:12: (at T 0) is part of a constant; it cannot be set");
    ExpectError(results, "(struct P (x u8))\n(fn f (args (p (ptr P))) (set (. (load p) x) 1))",
                "t.tsp:2:31: set does not write through pointers; write (store p value) with the "
                "whole value");
    ExpectError(results, "(fn f (args (x u8)) (set (+ x 1) 2))",
                "t.tsp:1:26: set needs a variable, s.x, (at ...) or (. ...); (+ x 1) is not one");
    ExpectError(results, t + "(let u (array u8 10))\n(fn f (set t u))",
                "t.tsp:3:7: arrays are copied element by element; use a for loop");
    ExpectError(results, "(fn f (args (p (ptr u8))) (store p 300))",
                "t.tsp:1:36: 300 does not fit in u8 (0 to 255)");
}

void DeclarationTests(TestResults& results) {
    ExpectError(results, "(fn g (returns u8) 1)\n(let x u8 (g))",
                "t.tsp:2:11: a top-level let starts with a constant value; set x at the start of "
                "main instead");
    ExpectError(results, "(const T (array u8 3) 1 2)",
                "t.tsp:1:1: T is (array u8 3) but has 2 values; give exactly 3");
    ExpectError(results, "(let x)", "t.tsp:1:1: let x needs a type or a value; write (let x u8 0)");
    ExpectError(results, "(const A u8 B)\n(const B u8 A)",
                "t.tsp:1:1: constant A depends on itself; give it a value that does not use A");
    ExpectError(results, "(+ 1 2)",
                "t.tsp:1:1: only import, const, let, struct, enum, fn and macro go at top "
                "level; put (+ ...) inside a fn");
    ExpectError(results, "(fn f (let x u8 0 :align 2))",
                "t.tsp:1:19: placement options (:address, :align, :boundary) are for top-level "
                "lets; move x out of the function");
    ExpectError(results, "(let x u8 0 :in zero_page)",
                "t.tsp:1:13: :in (named regions) is not in this subset yet; remove it");
    ExpectError(results, "(let x u8 0 :align 0)",
                "t.tsp:1:20: :align 0 means nothing; use 1 or more, like :align 256");
    ExpectError(results, "(let t (array u8 3) 1)",
                "t.tsp:1:21: arrays start zeroed; fill t in a fn, or write (const NAME (array t "
                "n) v0 v1 ...)");
    ExpectError(results, "(let t (array u8 0))",
                "t.tsp:1:18: an array has 1 to 65535 elements, not 0");
    ExpectError(results, "(fn f (const K u8 1))",
                "t.tsp:1:7: const goes at top level; use let here");
    ExpectError(results, "(const D u8 (/ 1 0))", "t.tsp:1:13: division by zero in a constant");
    // constants fold in any order, through other modules
    Built folded = BuildText("(const B u8 (+ A 1))\n(const A u8 (* 2 3))\n(let t (array u8 B))");
    CHECK(results, folded.ok);
}

} // namespace

void RunCheckFormTests(TestResults& results) {
    ControlTests(results);
    ReturnTests(results);
    MatchTests(results);
    LoopTests(results);
    PlaceTests(results);
    DeclarationTests(results);
}

} // namespace tisple_tests
