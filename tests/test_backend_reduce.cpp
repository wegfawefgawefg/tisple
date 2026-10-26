// strength reduction on the 6502 against the interpreter: lets of a u8 loop's body that
// move by a constant each iteration, worked out once and stepped, at the edges: a byte part
// that would wrap, a base the body changes, a let set later, loops that exit early

#include "test_backend.hpp"

#include <string>

namespace tisple_tests {

namespace {

// rows of wb through a row_of like Life's, read and written
const char* const kRows = "(let wb (array u8 1024))\n"
                          "(let base (ptr u8))\n"
                          "(let sum u16 0)\n"
                          "(fn row_of ((world (ptr u8)) (y u8)) (ptr u8)\n"
                          "    (+ world (* (as u16 y) 37)))\n"
                          "(fn bump () void (set base (+ base 2)))\n";

void CheckRows(TestResults& results, const std::string& body, std::uint16_t arg = 0) {
    std::string text = std::string(kRows) + "(fn f ((n u8)) void\n" +
                       "    (set base (addr_of (at wb 3)))\n" +
                       "    (for i u16 0 1024 (set (at wb i) (as u8 (* i 7))))\n" + body + ")\n";
    CheckProgram(results, text, "t.f", {arg});
}

void TestRowLets(TestResults& results) {
    // the Life shape: rows above, here and below; the counter in a register
    CheckRows(results, "    (for y u8 1 20\n"
                       "        (let above (ptr u8) (row_of base (- y 1)))\n"
                       "        (let here (ptr u8) (row_of base y))\n"
                       "        (let below (ptr u8) (row_of base (+ y 1)))\n"
                       "        (store here (+ (load above) (load below)))\n"
                       "        (store (+ above 1) (+ (load (+ below 2)) 1))\n"
                       "        (set sum (+ sum (as u16 (load (+ here 5))))))");
    // y - 1 from 0 wraps to 255, and y + 1 at 255 to 0: not straight lines
    CheckRows(
        results,
        "    (for y u8 0 20\n"
        "        (let a u16 (* (as u16 (- y 1)) 37))\n"
        "        (let here (ptr u8) (row_of base y))\n"
        "        (set sum (+ sum (+ (^ a (>> a 3)) (as u16 (+ (load here) (load (+ here 1))))))))");
    CheckRows(results, "    (for y u8 240 256\n"
                       "        (let a u16 (* (as u16 (+ y 1)) 300))\n"
                       "        (let b u16 (+ (as u16 (- y 240)) (as u16 (* (- y 240) 16))))\n"
                       "        (set sum (+ sum (^ (^ a b) (+ a (>> b 2))))))");
    // a variable end: y + 1 could pass 255, y alone cannot
    CheckRows(
        results,
        "    (for y u8 0 n\n"
        "        (let here (ptr u8) (row_of base y))\n"
        "        (let w u16 (+ (* (as u16 (+ y 1)) 300) (as u16 y)))\n"
        "        (set sum (+ (+ sum (^ w (>> w 1))) (as u16 (+ (load here) (load (+ here 1)))))))",
        20);
    CheckRows(results,
              "    (for y u8 0 n\n"
              "        (let here (ptr u8) (row_of base y))\n"
              "        (set sum (+ sum (as u16 (+ (load here) (load (+ here 1)))))))",
              0);
    // the base changes in the body, directly and through a call
    CheckRows(results, "    (for y u8 1 10\n"
                       "        (let here (ptr u8) (row_of base y))\n"
                       "        (set sum (+ sum (as u16 (+ (load here) (load (+ here 1))))))\n"
                       "        (set base (+ base 1)))");
    CheckRows(results, "    (for y u8 1 10\n"
                       "        (let here (ptr u8) (row_of base y))\n"
                       "        (set sum (+ sum (as u16 (+ (load here) (load (+ here 1))))))\n"
                       "        (bump))");
    // the base read through a pointer the body stores through
    CheckRows(results, "    (let pb (ptr (ptr u8)) (addr_of base))\n"
                       "    (for y u8 1 10\n"
                       "        (let here (ptr u8) (+ (load pb) (as u16 (* y 5))))\n"
                       "        (set sum (+ sum (as u16 (+ (load here) (load (+ here 1))))))\n"
                       "        (store pb (+ (load pb) 1)))");
    // the let set later in the body; continue and break before and after it
    CheckRows(results, "    (for y u8 1 10\n"
                       "        (let here (ptr u8) (row_of base y))\n"
                       "        (if (= y 4) (set here (addr_of (at wb 0))))\n"
                       "        (set sum (+ sum (as u16 (+ (load here) (load (+ here 1)))))))");
    CheckRows(results, "    (for y u8 1 25\n"
                       "        (if (= (& y 3) 1) (continue))\n"
                       "        (let here (ptr u8) (row_of base (+ y 2)))\n"
                       "        (set sum (+ sum (as u16 (+ (load here) (load (+ here 1))))))\n"
                       "        (if (= y 22) (break)))");
    // shifts, a product that wraps mod 65536, pointer minus, a byte product near 255
    CheckRows(results,
              "    (for y u8 0 30\n"
              "        (let a u16 (<< (as u16 y) 9))\n"
              "        (let b u16 (* (as u16 y) 3001))\n"
              "        (let p (ptr u8) (- (addr_of (at wb 900)) (as u16 (<< y 3))))\n"
              "        (let q (ptr u8) (+ (addr_of (at wb 0)) (as u16 (* y 8))))\n"
              "        (set sum (+ (+ sum (^ a b)) (as u16 (+ (load p) (load q)))))\n"
              "        (set sum (+ (+ sum (+ a b)) (as u16 (^ (load (+ p 1)) (load (+ q 2)))))))");
    CheckRows(results, "    (for y u8 0 90\n"
                       "        (let q (ptr u8) (+ (addr_of (at wb 0)) (as u16 (* y 3))))\n"
                       "        (set sum (+ sum (as u16 (+ (load q) (load (+ q 1)))))))");
    // a nested loop: the outer counter in memory, the lets of both loops
    CheckRows(results,
              "    (for y u8 1 8\n"
              "        (let here (ptr u8) (row_of base y))\n"
              "        (for x u8 0 4\n"
              "            (let cell (ptr u8) (+ here (as u16 (* x 5))))\n"
              "            (for k u8 0 2 (store cell (+ (load cell) k)))\n"
              "            (set sum (+ sum (as u16 (^ (load cell) (load (+ cell 1))))))))");
}

} // namespace

void RunBackendReduceTests(TestResults& results) {
    TestRowLets(results);
}

} // namespace tisple_tests
