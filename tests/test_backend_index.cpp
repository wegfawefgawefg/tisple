// byte indices on pointers and arrays: (p),Y with Y = e, Y kept across accesses, loop
// pointers for counter + k, lets that only name an index; each against the interpreter

#include "backend/index_alias.hpp"
#include "test_backend.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

// instructions of one mnemonic in a function's code
int CountMn(const Compiled& c, const std::string& function, tisple::Mn mn) {
    int count = 0;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        if (span.name != function) {
            continue;
        }
        for (std::size_t i = span.first; i < span.end; i++) {
            const tisple::AsmItem& item = c.code.as.items[i];
            count += item.kind == tisple::ItemKind::Ins && item.mn == mn ? 1 : 0;
        }
    }
    return count;
}

// the index edges: 0, 1, 127, 128, 254, 255; every pointer low byte, so (p),Y crosses a
// page for some of them
void TestIndexEdges(TestResults& results) {
    const char* text =
        "(const K u8 3)\n"
        "(let buf (array u8 1024))\n"
        "(let sum u16 0)\n"
        "(fn f ((off u8) (i u8) (v u8)) u16\n"
        "    (let p (ptr u8) (addr_of (at buf (as u16 off))))\n"
        "    (store (+ p (as u16 i)) v)\n"
        "    (store (+ p (as u16 (+ i 1))) (+ v 1))\n"
        "    (store (+ p (as u16 (- i 2))) (+ v 2))\n"
        "    (store (+ p (as u16 (+ i 200))) (+ v 3))\n"
        "    (store (+ p 255) (+ v 4))\n"
        "    (store (+ p (as u16 (+ K 4))) (+ v 5))\n"
        "    (let a u16 (as u16 (load (+ p (as u16 (- i 1))))))\n"
        "    (let b u16 (as u16 (load (+ p (as u16 i)))))\n"
        "    (let c u16 (as u16 (load (+ p (as u16 (+ i 1))))))\n"
        "    (let d u16 (as u16 (load (+ p (as u16 (+ 2 i))))))\n"
        "    (let e u16 (as u16 (load (+ p (as u16 (- i 2))))))\n"
        "    (let k u16 (as u16 (load (+ p (as u16 (+ i 200))))))\n"
        "    (let m u16 (as u16 (load (+ p 255))))\n"
        "    (set sum (+ sum (+ a (+ (* b 3) (+ (* c 5) (+ (* d 7) (+ (* e 11) (* k 13))))))))\n"
        "    (+ sum (* m 17)))\n";
    Compiled c = CompileText(text);
    Diff d;
    if (!StartDiff(d, c)) {
        std::fprintf(stderr, "index edges: %s\n", d.message.c_str());
        CHECK(results, false);
        return;
    }
    const std::uint16_t indices[] = {0, 1, 2, 127, 128, 199, 200, 254, 255};
    for (std::uint16_t off = 0; off < 256; off += 17) {
        for (std::uint16_t i : indices) {
            CheckDiff(results, d, "t.f", {off, i, static_cast<std::uint16_t>((off + i) & 0xFF)});
        }
    }
    // x - 1, x, x + 1 through one pointer is one LDY and some INY/DEY
    Compiled y = CompileText("(fn f ((p (ptr u8)) (x u8)) u8\n"
                             "    (+ (load (+ p (as u16 (- x 1))))\n"
                             "       (+ (load (+ p (as u16 x))) (load (+ p (as u16 (+ x 1)))))))");
    CHECK(results, y.ok);
    CHECK(results, CountMn(y, "t.f", tisple::Mn::LDY) == 1);
    CHECK(results, CountMn(y, "t.f", tisple::Mn::INY) == 2);
    CHECK(results, CountMn(y, "t.f", tisple::Mn::DEY) == 1);
}

// u16 element pointers and arrays indexed by a byte plus a constant
void TestIndexOther(TestResults& results) {
    const char* text = "(let ws (array u16 300))\n"
                       "(let bs (array u8 256))\n"
                       "(fn f ((i u8) (j u8)) u16\n"
                       "    (for k u8 0 255 (set (at bs k) (* k 3)))\n"
                       "    (set (at bs 255) 1)\n"
                       "    (let q (ptr u16) (addr_of (at ws 20)))\n"
                       "    (store (+ q (as u16 i)) 1000)\n"
                       "    (store (+ q (as u16 (+ i 1))) 2000)\n"
                       "    (+ (+ (load (+ q (as u16 i))) (load (+ q (as u16 (- j 1)))))\n"
                       "       (as u16 (+ (at bs (- i 1)) (+ (at bs (+ i 2)) (at bs j))))))";
    Compiled c = CompileText(text);
    Diff d;
    if (!StartDiff(d, c)) {
        std::fprintf(stderr, "index other: %s\n", d.message.c_str());
        CHECK(results, false);
        return;
    }
    const std::uint16_t indices[] = {0, 1, 2, 100, 253, 254, 255};
    for (std::uint16_t i : indices) {
        for (std::uint16_t j : indices) {
            CheckDiff(results, d, "t.f", {static_cast<std::uint16_t>(i % 200), j});
        }
    }
}

// loops: the counter in Y, (p + k),Y loop pointers, ranges that wrap and ones that do not,
// bounds known and not, nested loops over two pointers
void TestIndexLoops(TestResults& results) {
    const char* text =
        "(let src (array u8 600))\n"
        "(let dst (array u8 600))\n"
        "(let total u16 0)\n"
        "(fn fill () void\n"
        "    (for i u16 0 600 (set (at src i) (as u8 (+ (* i 7) (>> i 3))))))\n"
        "(fn f ((lo u8) (hi u8) (off u8)) u16\n"
        "    (let p (ptr u8) (addr_of (at src (as u16 off))))\n"
        "    (let q (ptr u8) (addr_of (at dst (as u16 off))))\n"
        "    (for x u8 1 255\n"
        "        (store (+ q (as u16 x))\n"
        "            (+ (load (+ p (as u16 (- x 1)))) (load (+ p (as u16 (+ x 1)))))))\n"
        "    (for x u8 0 255\n"
        "        (set total (+ total (as u16 (load (+ p (as u16 (- x 1))))))))\n"
        "    (for x u8 250 256\n"
        "        (set total (+ total (as u16 (load (+ q (as u16 (+ x 1))))))))\n"
        "    (for x u8 lo hi\n"
        "        (store (+ q (as u16 (+ x 2))) (load (+ p (as u16 (- x 2))))))\n"
        "    (for y u8 1 4\n"
        "        (let row (ptr u8) (+ p (as u16 (* y 40))))\n"
        "        (let out (ptr u8) (+ q (as u16 (* y 40))))\n"
        "        (for x u8 1 39\n"
        "            (store (+ out (as u16 x))\n"
        "                (+ (load (+ row (as u16 (- x 1))))\n"
        "                    (+ (load (+ row (as u16 y))) (load (+ p (as u16 (+ y x)))))))))\n"
        "    (+ total (as u16 (load (+ q 100)))))";
    Compiled c = CompileText(text);
    Diff d;
    if (!StartDiff(d, c)) {
        std::fprintf(stderr, "index loops: %s\n", d.message.c_str());
        CHECK(results, false);
        return;
    }
    CheckDiff(results, d, "t.fill", {});
    CheckDiff(results, d, "t.f", {0, 255, 0});
    CheckDiff(results, d, "t.f", {3, 40, 77});
    CheckDiff(results, d, "t.f", {250, 255, 255});
    CheckDiff(results, d, "t.f", {9, 5, 128});
}

// Y is kept across accesses only while nothing can change it or what it was loaded from:
// calls, stores through pointers that reach the index, other indexed code, inner loops
void TestYReuse(TestResults& results) {
    const char* text =
        "(let buf (array u8 512))\n"
        "(let g u8 0)\n"
        "(fn clobber ((p (ptr u8))) u8\n"
        "    (let s u8 0)\n"
        "    (for i u8 0 9 (set s (+ s (load (+ p (as u16 (* i 3)))))))\n"
        "    (set g (+ g s))\n"
        "    (for i u8 0 9 (store (+ p (as u16 (+ i 100))) (+ s i)))\n"
        "    (set s (^ s (load (+ p (as u16 (+ g 1))))))\n"
        "    (+ s (load (+ p (as u16 (- g 1))))))\n"
        "(fn f ((i u8)) u16\n"
        "    (let p (ptr u8) (addr_of (at buf 3)))\n"
        "    (let k u8 i)\n"
        "    (let pk (ptr u8) (addr_of k))\n"
        "    (store (+ p (as u16 k)) 5)\n"
        "    (let a u8 (+ (load (+ p (as u16 k))) (+ (clobber p) (load (+ p (as u16 (+ k 1)))))))\n"
        "    (store pk (+ k 9))\n"
        "    (let b u8 (load (+ p (as u16 k))))\n"
        "    (store (+ p (as u16 (+ g 2))) b)\n"
        "    (set g (+ g 1))\n"
        "    (let c u8 (load (+ p (as u16 (+ g 2)))))\n"
        "    (for y u8 0 3\n"
        "        (for x u8 0 4\n"
        "            (store (+ p (as u16 (+ x y))) (+ (load (+ p (as u16 (+ k x)))) y))))\n"
        "    (+ (as u16 a) (+ (* (as u16 b) 256) (as u16 (+ c (load (+ p (as u16 k))))))))";
    Compiled c = CompileText(text);
    Diff d;
    if (!StartDiff(d, c)) {
        std::fprintf(stderr, "y reuse: %s\n", d.message.c_str());
        CHECK(results, false);
        return;
    }
    for (int i : {0, 1, 100, 200, 245, 254, 255}) {
        CheckDiff(results, d, "t.f", {static_cast<std::uint16_t>(i)});
    }
    // a store through (pk),Y with Y = k changes k itself (k = 0, so pk + k is k): the next
    // index k loads Y again
    CheckProgram(results,
                 "(let buf (array u8 64))\n"
                 "(fn f ((i u8)) u16\n"
                 "    (for j u8 0 64 (set (at buf j) (* j 5)))\n"
                 "    (let p (ptr u8) (addr_of (at buf 1)))\n"
                 "    (let k u8 i)\n"
                 "    (let pk (ptr u8) (addr_of k))\n"
                 "    (let a u8 (load (+ p (as u16 k))))\n"
                 "    (store (+ pk (as u16 k)) (+ k 9))\n"
                 "    (let b u8 (load (+ p (as u16 k))))\n"
                 "    (+ (as u16 a) (* (as u16 b) 256)))",
                 "t.f", {0});
}

// lets that only name an index: skipped when nothing else reads them, kept when something
// does, not aliases when what they read changes in their scope
void TestIndexLets(TestResults& results) {
    const char* text = "(let buf (array u8 512))\n"
                       "(fn f ((i u8) (n u8)) u16\n"
                       "    (let p (ptr u8) (addr_of (at buf 9)))\n"
                       "    (for x u8 0 n (store (+ p (as u16 x)) (^ x 0x5A)))\n"
                       "    (let s u16 0)\n"
                       "    (for x u8 1 40\n"
                       "        (let l u16 (as u16 (- x 1)))\n"
                       "        (let r u16 (as u16 (+ x 1)))\n"
                       "        (let m u8 (+ x 3))\n"
                       "        (set s (+ s (as u16 (+ (load (+ p l)) (load (+ p r))))))\n"
                       "        (set s (+ s (as u16 (load (+ p (as u16 m))))))\n"
                       "        (set s (+ s r)))\n"
                       "    (let j u8 i)\n"
                       "    (let k u16 (as u16 (+ j 2)))\n"
                       "    (set j 7)\n"
                       "    (let t u16 (as u16 (load (+ p k))))\n"
                       "    (let w u16 (as u16 (- i 1)))\n"
                       "    (+ s (+ t (as u16 (load (+ p w))))))";
    Compiled c = CompileText(text);
    Diff d;
    if (!StartDiff(d, c)) {
        std::fprintf(stderr, "index lets: %s\n", d.message.c_str());
        CHECK(results, false);
        return;
    }
    for (int i : {0, 1, 7, 128, 255}) {
        CheckDiff(results, d, "t.f", {static_cast<std::uint16_t>(i), 255});
        CheckDiff(results, d, "t.f", {static_cast<std::uint16_t>(i), 3});
    }
    const tisple::Program& program = c.built.program;
    std::int32_t l = -1;
    std::int32_t r = -1;
    std::int32_t k = -1;
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const std::string& name = program.vars[v].name;
        std::int32_t id = static_cast<std::int32_t>(v);
        l = name == "l" ? id : l;
        r = name == "r" ? id : r;
        k = name == "k" ? id : k;
    }
    std::vector<tisple::IndexAlias> aliases = tisple::FindIndexAliases(program);
    CHECK(results, l >= 0 && aliases[static_cast<std::size_t>(l)].index_only);
    CHECK(results, r >= 0 && aliases[static_cast<std::size_t>(r)].value >= 0 &&
                       !aliases[static_cast<std::size_t>(r)].index_only);
    CHECK(results, k >= 0 && aliases[static_cast<std::size_t>(k)].value < 0);
}

// lets read once move to their read: lazily under and/or, never past a write to what they
// read
void TestForwardLets(TestResults& results) {
    const char* text = "(let buf (array u8 16))\n"
                       "(let g u8 0)\n"
                       "(let h u16 0)\n"
                       "(fn f ((x u8)) u16\n"
                       "    (for i u8 0 16 (set (at buf i) (+ i x)))\n"
                       "    (let p (ptr u8) (addr_of (at buf 2)))\n"
                       "    (let lazy u8 (load (+ p (as u16 x))))\n"
                       "    (let flag bool (< (load p) 9))\n"
                       "    (if (or (= x 1) (and flag (= lazy 3))) (set g (+ g 1)))\n"
                       "    (let old u8 (load p))\n"
                       "    (store p 99)\n"
                       "    (set g (+ g old))\n"
                       "    (let y u8 (+ x 1))\n"
                       "    (set x 5)\n"
                       "    (set g (+ g y))\n"
                       "    (let arm u8 (load (+ p 1)))\n"
                       "    (if (> x 3) (set g (+ g arm)))\n"
                       "    (let wide u16 (+ (as u16 g) 300))\n"
                       "    (store (+ p 2) (as u8 (>> wide 1)))\n"
                       "    (let looped u8 (at buf 5))\n"
                       "    (for i u8 0 3 (set h (+ h (as u16 looped))))\n"
                       "    (+ h (as u16 g)))";
    Compiled c = CompileText(text);
    Diff d;
    if (!StartDiff(d, c)) {
        std::fprintf(stderr, "forward lets: %s\n", d.message.c_str());
        CHECK(results, false);
        return;
    }
    for (int x : {0, 1, 2, 3, 7, 200}) {
        CheckDiff(results, d, "t.f", {static_cast<std::uint16_t>(x)});
    }
    const tisple::Program& program = c.built.program;
    std::vector<tisple::IndexAlias> aliases = tisple::FindIndexAliases(program);
    auto forward = [&](const char* name) {
        for (std::size_t v = 0; v < program.vars.size(); v++) {
            if (program.vars[v].name == name) {
                return aliases[v].forward;
            }
        }
        return false;
    };
    CHECK(results, forward("lazy") && forward("flag") && forward("wide"));
    CHECK(results, !forward("old") && !forward("y") && !forward("arm") && !forward("looped"));
}

// a run of + over plain reads goes once through A, with one leaf that needs code first
void TestChains(TestResults& results) {
    const char* text = "(let a (array u8 8))\n"
                       "(let g u8 9)\n"
                       "(fn h ((x u8)) u8 (set g (+ g x)) (* g 3))\n"
                       "(fn f ((x u8) (y u8)) u8\n"
                       "    (for i u8 0 8 (set (at a i) (* i 37)))\n"
                       "    (let s u8 (+ (+ x (* y 3)) (+ (at a 2) (+ y g))))\n"
                       "    (let t u8 (| (| x (at a (& y 7))) (| (& x y) g)))\n"
                       "    (let u u8 (+ (+ (h x) g) (+ (h y) g)))\n"
                       "    (^ (+ s t) (+ u (+ (+ x 255) (+ y 1)))))";
    Compiled c = CompileText(text);
    Diff d;
    if (!StartDiff(d, c)) {
        std::fprintf(stderr, "chains: %s\n", d.message.c_str());
        CHECK(results, false);
        return;
    }
    for (int x : {0, 1, 77, 255}) {
        CheckDiff(results, d, "t.f",
                  {static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(255 - x)});
    }
}

} // namespace

void RunBackendIndexTests(TestResults& results) {
    TestIndexEdges(results);
    TestIndexOther(results);
    TestIndexLoops(results);
    TestYReuse(results);
    TestIndexLets(results);
    TestForwardLets(results);
    TestChains(results);
}

} // namespace tisple_tests
