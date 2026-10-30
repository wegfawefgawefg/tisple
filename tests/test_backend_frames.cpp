// Bytes for the locals of inlined copies (backend/frame_share.hpp): copies of functions called
// one after the other share bytes, as the calls' frames did, and the copies' locals go into
// the frames of the functions they were copied from; copies whose values meet, or a copy's
// value across a call of a function whose frame may overlap, keep bytes apart. Each program
// also runs from main on the 6502 against the interpreter, which runs it as written.

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

void CheckMain(TestResults& results, const std::string& text) {
    Compiled c = CompileMainText(text);
    Diff d;
    bool ok = c.ok && StartDiff(d, c) && DiffCall(d, "t.main", {});
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s%s\n", text.c_str(), d.message.c_str(),
                     c.errors.c_str());
    }
    CHECK(results, ok);
}

// three small functions copied five times into main's loop: their locals share bytes, so the
// frames are those of the calls (before, every copy's locals had bytes of their own in main's
// frame: 16 zero-page frame bytes and 12 in memory)
void Sizes(TestResults& results) {
    const std::string text =
        "(let tv (array u8 8) (make (array u8 8) 3 9 200 17 0 255 128 64))\n"
        "(let acc u16 0)\n(let g u8 0)\n"
        "(fn a ((x u8)) u8 (let t u8 (+ x 7)) (let u u8 (^ t (>> x 1))) (+ (* t u) x))\n"
        "(fn b ((x u8) (y u8)) u8 (let t u8 (- x y)) (let s u8 (+ (* t t) y)) (^ s g))\n"
        "(fn c ((p u16)) u16 (let q u16 (+ p 1000)) (let r u16 (* q 3)) (+ r (>> p 2)))\n"
        "(fn main () u16\n"
        "    (for i u8 0 20\n"
        "        (let v u8 (at tv (& i 7)))\n"
        "        (set g (a v))\n"
        "        (set g (b g (a (+ v 1))))\n"
        "        (set acc (+ acc (c (as u16 g))))\n"
        "        (set acc (^ acc (c acc))))\n"
        "    acc)";
    Compiled c = CompileMainText(text);
    tisple::OptOptions calls;
    calls.disabled = {"inline"};
    Compiled called = CompileMainText(text, calls);
    CHECK(results, c.ok && called.ok);
    // every call copied: a, b and c have no code
    bool copied = true;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        copied = copied && span.name != "t.a" && span.name != "t.b" && span.name != "t.c";
    }
    CHECK(results, copied);
    std::uint32_t copies = c.code.zp_frames + c.code.mem_frames;
    std::uint32_t backend = called.code.zp_frames + called.code.mem_frames;
    if (c.code.zp_frames != 8 || c.code.mem_frames != 4 || copies > backend) {
        std::fprintf(stderr, "frames: %u zero page, %u memory (the backend's inlining: %u, %u)\n",
                     c.code.zp_frames, c.code.mem_frames, called.code.zp_frames,
                     called.code.mem_frames);
    }
    CHECK(results, c.code.zp_frames == 8 && c.code.mem_frames == 4 && copies <= backend);
    CheckMain(results, text);
}

// copies whose bytes could share and must not, or may
void Meets(TestResults& results) {
    const char* const helpers =
        "(let g u8 7)\n(let h u16 300)\n"
        "(let rt (array u8 16) (make (array u8 16) 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16))\n"
        "(fn ra ((x u8) (y u8)) u8 (let t u8 (+ x g)) (let u u8 (^ t y)) (+ (* t 3) u))\n"
        "(fn rv ((x u8)) u8 (let t u8 (+ x g)) (set h (+ h (as u16 t))) t)\n"
        "(fn rp ((k u8)) u8 (let p (ptr u8) (addr_of (at rt (& k 15)))) (+ (load p) k))\n"
        "(fn rw ((x u8)) u16 (let w u16 (* (as u16 x) 37)) (+ w h))\n"
        // a JSR: small with a loop that stores, called in several places
        "(fn rl ((n u8)) u8 (let acc u8 1) (for i u8 0 (& n 7) (set acc (+ acc (at rt i)))\n"
        "    (set (at rt (& (+ i n) 15)) acc)) (set h (+ h (as u16 acc))) acc)\n"
        "(fn rz () u8 (let acc u8 g) (for i u8 0 (& (as u8 h) 7) (set acc (+ acc (at rt i)))\n"
        "    (set (at rt (& (+ i g) 15)) acc)) (set g (+ acc 1)) acc)\n";
    const char* const mains[] = {
        // copies of one function in each other's arguments
        "(fn main () u8 (for i u8 0 9 (set g (ra (ra i g) (ra g i)))) g)",
        // values of copies of different functions needed at once
        "(fn main () u8 (for i u8 0 9 (set h (+ (rw (ra i g)) (rw (rv i))))) (as u8 h))",
        // a copy's value, its own local, across the call of a JSR whose frame overlaps
        "(fn main () u8 (for i u8 0 9 (set g (+ (rv i) (rz))) (set g (^ (rv g) (rl i)))) g)",
        // pointer locals in zero page, copied many times
        "(fn main () u8 (for i u8 0 9 (set g (+ (rp i) (rp (ra i g))))) g)",
        // copies in a function called once, around calls, with copies in main too
        ("(fn once ((x u8)) u8 (let a u8 (ra x g)) (let b u8 (rl a)) (+ a (+ b (rp (rv b)))))\n"
         "(fn main () u8 (set g (ra g 3)) (set g (+ g (once g))) (set h (rw (rv g))) g)"),
        // a local of main live across copies: never shared with them
        "(fn main () u8 (let keep u8 (rz)) (for i u8 0 5 (set g (ra (rv i) keep))) (+ keep g))",
    };
    for (const char* text : mains) {
        CheckMain(results, std::string(helpers) + text);
    }
}

} // namespace

void RunBackendFrameTests(TestResults& results) {
    Sizes(results);
    Meets(results);
}

} // namespace tisple_tests
