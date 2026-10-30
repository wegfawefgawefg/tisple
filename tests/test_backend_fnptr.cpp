// Function pointers: calls through them on the 6502 against the interpreter, at -O1 and -O0,
// and the mid-level passes treating a function whose address is taken as called from
// anywhere: no parameter ranges, return values or pointer targets from the calls the tree
// shows, and effects of every function a pointer may hold

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

// t.main on both, with the passes and without
void CheckMain(TestResults& results, const std::string& text) {
    for (int level : {1, 0}) {
        tisple::OptOptions opt;
        opt.level = level;
        Compiled c = CompileMainText(text, opt);
        Diff d;
        bool ok = StartDiff(d, c) && DiffCall(d, "t.main", {});
        if (!ok) {
            std::fprintf(stderr, "program at -O%d:\n%s\n  %s\n", level, text.c_str(),
                         d.message.c_str());
        }
        CHECK(results, ok);
    }
}

// the u16 global out after t.main, on the 6502
std::uint16_t OutAfterMain(const std::string& text) {
    Compiled c = CompileMainText(text);
    Diff d;
    if (!StartDiff(d, c) || !DiffCall(d, "t.main", {})) {
        std::fprintf(stderr, "program:\n%s\n  %s\n", text.c_str(), d.message.c_str());
        return 0xFFFF;
    }
    const tisple::VarPlace& place =
        c.code.layout.vars[static_cast<std::size_t>(tisple::FindVar(c.built.program, "t.out"))];
    return static_cast<std::uint16_t>(d.sim.machine->mem[place.address] |
                                      (d.sim.machine->mem[place.address + 1] << 8));
}

void DispatchTests(TestResults& results) {
    // tables, fields, arguments, returns, nesting
    CheckMain(results, R"(
(let out u16 0)
(fn add_one ((x u8)) u8 (+ x 1))
(fn double ((x u8)) u8 (* x 2))
(fn on_a () void (set out (+ out 10)))
(fn on_b () void (set out (+ out 200)))
(const OPS (array (fn (u8) u8) 2) (make (array (fn (u8) u8) 2) add_one double))
(const EVENTS (array (fn () void) 2) (make (array (fn () void) 2) on_a on_b))
(struct Obj ((hp u8) (update (fn (u8) u8))))
(let objs (array Obj 3))
(fn apply ((f (fn (u8) u8)) (v u8)) u8 (f v))
(fn pick ((k u8)) (fn (u8) u8) (if (= k 0) add_one double))
(fn main () void
    (let f (fn (u8) u8) add_one)
    (set out (as u16 (f 5)))
    (for i u8 0 2
        (let op (fn (u8) u8) (at OPS i))
        (set out (+ out (as u16 (op 20))))
        (let ev (fn () void) (at EVENTS i))
        (ev))
    (set (at objs 0 update) double)
    (set (at objs 1 update) add_one)
    (set (at objs 2 update) (at OPS 1))
    (for i u8 0 3
        (let u (fn (u8) u8) (at objs i update))
        (set (at objs i hp) (u (+ i 7))))
    (let g (fn (u8) u8) (pick 1))
    (set out (+ out (as u16 (apply double (g (f 9))))))
    (if (= g double) (set out (+ out 1000))))
)");
    // 16-bit and bool results, pointer arguments, two arguments with a call in the second
    CheckMain(results, R"(
(let out u16 0)
(let cells (array u8 8))
(fn wide ((a u16) (b u8)) u16 (+ (* a 3) (as u16 b)))
(fn mix ((a u16) (b u8)) u16 (- a (as u16 b)))
(fn odd ((p (ptr u8))) bool (store p (+ (load p) 3)) (= (& (load p) 1) 1))
(const WIDE (array (fn (u16 u8) u16) 2) (make (array (fn (u16 u8) u16) 2) wide mix))
(fn main () void
    (for i u8 0 2
        (let w (fn (u16 u8) u16) (at WIDE i))
        (let inner (fn (u16 u8) u16) (at WIDE (- 1 i)))
        (set out (+ out (w 1000 (as u8 (inner 40 (+ i 1)))))))
    (let t (fn ((ptr u8)) bool) odd)
    (for i u8 0 8
        (if (t (addr_of (at cells i)))
            (set out (+ out 1)))))
)");
    // a target making calls through a pointer of another type
    CheckMain(results, R"(
(let out u16 0)
(fn leaf ((x u8)) u8 (+ x 5))
(let inner (fn (u8) u8))
(fn outer ((x u16)) void (set out (+ out (as u16 (inner (as u8 x))))))
(fn main () void
    (set inner leaf)
    (let o (fn (u16) void) outer)
    (for i u16 0 300 (o i)))
)");
}

void ClosedWorldTests(TestResults& results) {
    // every direct call passes 2; the call through a pointer passes 200
    CHECK(results, OutAfterMain(R"(
(let out u16 0)
(fn scale ((k u8)) u16 (* (as u16 k) 3))
(fn main () void
    (set out (scale 2))
    (let f (fn (u8) u16) scale)
    (set out (+ out (f 200))))
)") == 606);
    // no direct call uses the value; the call through a pointer does
    CHECK(results, OutAfterMain(R"(
(let out u16 0)
(let g u8 0)
(fn bump () u8
    (set g (+ g 1))
    (* g 7))
(fn main () void
    (bump)
    (let f (fn () u8) bump)
    (set out (as u16 (f))))
)") == 14);
    // a target writes what the caller knows, worked out before the call or in the loop
    CHECK(results, OutAfterMain(R"(
(let out u16 0)
(let g u8 1)
(fn set_g () void (set g (+ g 9)))
(fn main () void
    (set g 1)
    (let f (fn () void) set_g)
    (let before u16 (* (as u16 g) 300))
    (f)
    (set out (+ before (* (as u16 g) 300)))
    (for i u8 0 3
        (set out (+ out (* (as u16 g) 7)))
        (f)))
)") == 300 + 3000 + 70 + 133 + 196);
    // a loop calls a function whose call through a pointer stores into what the loop reads
    CHECK(results, OutAfterMain(R"(
(let out u16 0)
(let arr (array u8 4))
(fn poke ((p (ptr u8))) void (store p (+ (load p) 1)))
(let hook (fn ((ptr u8)) void))
(fn run_hook () void (hook (addr_of (at arr 0))))
(fn main () void
    (set hook poke)
    (for i u8 0 5
        (run_hook)
        (set out (+ out (as u16 (at arr 0))))))
)") == 15);
    // the same with a store whose place provenance knows
    CHECK(results, OutAfterMain(R"(
(let out u16 0)
(let arr (array u8 4))
(fn poke () void (store (addr_of (at arr 0)) (+ (load (addr_of (at arr 0))) 1)))
(let hook (fn () void))
(fn run_hook () void (hook))
(fn main () void
    (set hook poke)
    (for i u8 0 5
        (run_hook)
        (set out (+ out (as u16 (at arr 0))))))
)") == 15);
    // a target's pointer parameter: direct calls pass a, the call through a pointer b, which a
    // loop reads (collapse) and whose element nothing else stores (elements)
    CHECK(results, OutAfterMain(R"(
(let out u16 0)
(let a u8 0)
(let b u8 0)
(let arr (array u8 8))
(fn bump ((p (ptr u8))) void (store p (+ (load p) 1)))
(let hook (fn ((ptr u8)) void))
(fn via () void (hook (addr_of b)))
(fn main () void
    (bump (addr_of a))
    (set hook bump)
    (for i u8 0 4
        (via)
        (set out (+ out (as u16 b))))
    (hook (addr_of (at arr 3)))
    (set out (+ out (* (as u16 (at arr 3)) 100))))
)") == 110);
    // a pointer parameter of a target: the calls through the pointer pass other places
    CHECK(results, OutAfterMain(R"(
(let out u16 0)
(let a u8 3)
(let b u8 40)
(fn read ((p (ptr u8))) u8 (load p))
(fn main () void
    (let x u8 (read (addr_of a)))
    (let f (fn ((ptr u8)) u8) read)
    (set b 41)
    (set out (+ (as u16 x) (as u16 (f (addr_of b))))))
)") == 44);
}

void InterpTests(TestResults& results) {
    // a call through a pointer that holds no function stops the interpreter
    Built built = BuildText(R"(
(let h (fn () void))
(fn main () void (h))
)");
    CHECK(results, built.ok);
    if (!built.ok) {
        return;
    }
    tisple::Layout layout;
    std::vector<tisple::Diagnostic> errors;
    CHECK(results, tisple::LayoutProgram(built.program, tisple::LayoutOptions(), layout, errors));
    tisple::Interp in;
    tisple::InitInterp(in, built.program, layout);
    tisple::InterpResult r = tisple::CallFunction(in, built.program.main, {});
    CHECK(results, r.kind == tisple::StopKind::Error &&
                       r.message.find("holds no function") != std::string::npos);
}

} // namespace

void RunBackendFnPtrTests(TestResults& results) {
    DispatchTests(results);
    ClosedWorldTests(results);
    InterpTests(results);
}

} // namespace tisple_tests
