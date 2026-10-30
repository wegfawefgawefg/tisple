// places reached through addresses the compiler knows: a global pointer nothing sets and a
// local pointer one let sets to (addr_of x) are x's address (opt/pointers.cpp), loads and
// stores through (addr_of x) read and write x directly (gen_place.cpp), and struct copies
// through pointers go by a loop (gen_struct.cpp)

#include "test_backend.hpp"

#include <cstdio>
#include <string>

namespace tisple_tests {

namespace {

const char* const kRecords =
    "(struct Rec ((next (ptr Rec)) (kind u8) (n i16) (name (array u8 9))))\n"
    "(let glob Rec)\n"
    "(let other Rec)\n"
    "(let recs (array Rec 4))\n"
    "(let words (array u16 6))\n"
    "(let pg (ptr Rec) (addr_of glob))\n"
    "(let pr (ptr Rec) (addr_of (at recs 2)))\n"
    "(let pw (ptr u16) (addr_of (at words 3)))\n"
    "(let out u16 :address 0x5F00)\n";

void TestFixedPointers(TestResults& results) {
    std::string text =
        std::string(kRecords) +
        "(fn bump ((p (ptr Rec)) (k i16)) void\n"
        "    (store p n (+ (load p n) k))\n"
        "    (store p kind (+ (load p kind) 1)))\n"
        "(fn main () void\n"
        "    (store pg next pr)\n"
        "    (store pg kind 7)\n"
        "    (bump pg 300)\n"
        "    (bump pr -5)\n"
        "    (bump (load pg next) 9)\n"
        "    (store pw (+ (load pw) 1000))\n"
        "    (store (+ pw 1) 77)\n"
        "    (let q (ptr Rec) (addr_of other))\n"
        "    (store q (load pg))\n"
        "    (store q kind (+ (load q kind) 1))\n"
        "    (set out (+ (+ (as u16 (load pg n)) (as u16 (at recs 2 n)))\n"
        "                (+ (+ (at words 3) (at words 4)) (as u16 other.kind)))))\n";
    for (int level = 0; level < 2; level++) {
        tisple::OptOptions opt;
        opt.level = level;
        Compiled c = CompileMainText(text, opt);
        Diff d;
        bool ok = StartDiff(d, c) && DiffCall(d, "t.main", {});
        if (!ok) {
            std::fprintf(stderr, "fixed pointers, -O%d:\n%s\n  %s\n", level, text.c_str(),
                         d.message.c_str());
        }
        CHECK(results, ok);
    }
}

// a pointer set somewhere, or one whose address is taken, stays a pointer
void TestChangingPointers(TestResults& results) {
    std::string text = std::string(kRecords) +
                       "(let moved (ptr Rec) (addr_of glob))\n"
                       "(fn main () void\n"
                       "    (store moved n 5)\n"
                       "    (set moved (addr_of other))\n"
                       "    (store moved n 6)\n"
                       "    (let q (ptr Rec) (addr_of glob))\n"
                       "    (if (= (at words 0) 0) (set q (addr_of other)))\n"
                       "    (store q kind 9)\n"
                       "    (set out (+ (as u16 glob.n) (+ (as u16 other.n) (as u16 other.kind)))))\n";
    CheckProgram(results, text, "t.main", {});
}

// struct copies through pointers: to itself, between elements, and to a static place
void TestLoopCopies(TestResults& results) {
    std::string text = std::string(kRecords) +
                       "(fn copy ((d (ptr Rec)) (s (ptr Rec))) void (store d (load s)))\n"
                       "(fn main () void\n"
                       "    (for i u8 0 4\n"
                       "        (store (addr_of (at recs i)) kind i)\n"
                       "        (store (addr_of (at recs i)) n (* (as i16 i) 1000))\n"
                       "        (store (addr_of (at recs i)) next (addr_of (at recs (& (+ i 1) 3)))))\n"
                       "    (copy (addr_of (at recs 1)) (addr_of (at recs 1)))\n"
                       "    (copy (addr_of (at recs 0)) (addr_of (at recs 3)))\n"
                       "    (copy (addr_of glob) (addr_of (at recs 2)))\n"
                       "    (store pr (load (addr_of (at recs 1))))\n"
                       "    (set out (+ (as u16 (at recs 0 n)) (as u16 glob.kind))))\n";
    for (int level = 0; level < 2; level++) {
        tisple::OptOptions opt;
        opt.level = level;
        Compiled c = CompileMainText(text, opt);
        Diff d;
        bool ok = StartDiff(d, c) && DiffCall(d, "t.main", {});
        if (!ok) {
            std::fprintf(stderr, "loop copies, -O%d:\n%s\n  %s\n", level, text.c_str(),
                         d.message.c_str());
        }
        CHECK(results, ok);
    }
}

} // namespace

void RunBackendPlacesTests(TestResults& results) {
    TestFixedPointers(results);
    TestChangingPointers(results);
    TestLoopCopies(results);
}

} // namespace tisple_tests
