# Dogfood log

Issues found while writing real programs in Tisple. Fixed in batches; each
entry says what happened and what it should do.

## Open

From a scan of Life, Snake, dot, the other examples and the libraries
(2026-10-26). Library and language ideas, not decided.

5. Every program reads the keyboard the same way: `(io_read u8 a2.KBD)`,
   return when it is under 0x80, `(io_read u8 a2.KBDSTRB)`, `(& key 0x7F)`
   (Life, Snake, dot, keys, read_key). A `read_key` in lib/apple2 that gives
   0 when no key is waiting would do, once a call that inlines costs nothing.
6. Life and Snake carry the same 16-bit xorshift `random`. A lib/random
   would share it.
7. `inc` and `dec` macros are written again in dot and macros. Macros
   import like everything else, so a library could hold them, and `when`
   too: a one-armed `if` with several forms needs `(if c (do a b))` (6 in
   Life, Snake and dot). Snake's step also nests `(if a (do ...) (if b
   ...))` for lack of `else if`; a match on named constants covers some of
   those.
8. examples/dot/gfx/hgr.tsp splits the row addresses into row_lo and
   row_hi by hand and joins them with `(| (<< (as u16 (at row_hi y)) 8)
   (as u16 (at row_lo y)))`. An `(array u16 192)` is already stored as
   two byte tables, so `(at row_start y)` would say it directly. Left as it
   is because the interpreter, backend and gap finder tests use the two
   tables.
9. Backend: `(let l u8 (- x 1))` and then `(+ above l)` (and the same for
   draw's `c`) compiles to 12 more bytes in Life than `(+ above (- x 1))`;
   the byte-index
   tracking doesn't see through a u8 let the way it does through
   `(let l u16 (as u16 (- x 1)))`.

## Fixed

1. `(store p 0)`: the literal gets no type, yet `(as u8 0)` is rejected as
   converting nothing. The stored value should take the pointer's element
   type as context, like `set` does. (lib/lores.tsp `clear`)
   Fixed 2026-10-26. The value already took the element type; the real
   pointer was `(store (+ p x) 0)` with `x : u8`, where the bad count made
   the pointer an error and the value was then checked with no type at all,
   so a second error blamed the literal. A value whose place or pointer
   failed (store, set, `as` with an unknown type) is now checked quietly.
   Every value slot gives a literal its type: store and field store (the
   element or field type), `set`, io_write, match patterns, array and struct
   values, call arguments, `return` (tests in test_check_types.cpp).
2. `(+ p x)` with `x : u8` must be written `(+ p (as u16 x))`. A `u8` index on
   a pointer is the most common 6502 idiom (`LDA (p),Y`); allow `u8` and `u16`
   counts directly.
   Fixed 2026-10-26 in the checker: a `u8` count gets the same Convert node
   `(as u16 x)` makes, so the backend sees the tree it already turns into
   `(p),Y`. Life, Snake, lib/lores.tsp and every example compile to the same
   bytes and the same listing as before (only the echoed source lines
   differ). The fuzz generator writes both spellings.
3. The formatter puts a `const` table's type on its own line and packs the
   values to 100 columns, losing hand grouping (8 row addresses per line in
   lib/lores.tsp). Keep the type on the head line; maybe keep the author's
   line breaks inside data lists.
   Fixed 2026-10-26: `let` and `const` keep the type on the head line, and a
   run of atoms in a list that takes several lines keeps the source's line
   breaks (rule in design.md, Syntax). lores' ROW_START and hires_plot's
   ROW_ADDR are 8 a line again. The format tests now cover programs/ and lib/
   too.
4. `(load (+ p (as u16 e)))` with `e : u8` and `p` in zero page computed a
   full 16-bit pointer sum per access (about 23 cycles) instead of `LDY e` /
   `LDA (p),Y`. Life's inner loop does 9 such reads per cell: about 520 cycles
   per cell, 1 generation per second. (programs/life)
   Fixed 2026-10-26 in the backend: a byte index (a u8 or the loop counter,
   plus or minus constants, widened) is (p),Y with Y tracked across accesses
   (x - 1, x, x + 1 is LDY / DEY / INY / INY); with the column counter in Y,
   `p + x - 1` reads through a loop pointer `p - 1` set once per row, so the
   inner loop has no LDY at all. `(let l u16 (as u16 (- x 1)))` lets that only
   index cost nothing. Step is 94 cycles a cell, draw 24 (cycle report).
   Since item 2, Life writes `(load (+ above (- x 1)))` with no index lets:
   the same instructions, 6 bytes less frame.

10. Found in the same scan and fixed 2026-10-26. `(let k u8 (if c 1))` gave
    two errors, "1 on its own does nothing" and "gives no value"; it is now
    one: an if without an else gives no value, write `(if c 1 other)`.
    `(for i u8 0 300)` said only that 300 does not fit in u8; it now says to
    write `(for i u16 0 300 ...)`. dot's hgr.flip and multi's lores.plot
    built a pointer as `(as (ptr u8) (+ row (as u16 x)))`; they now write
    `(+ (as (ptr u8) row) x)`, the same code.

5. Gap list item 1, division (2026-10-26). hires_plot's `(/ x 7)` and
   `(% x 7)` each called divu16 with a 16-bit divisor: about 750 cycles each.
   Now (src/backend/gen_div.cpp) unsigned division by a constant below 128
   that is not a power of two is long division with the divisor as an
   immediate, one unrolled step (`ROL q / ROL A / CMP #c / BCC / SBC #c`) per
   dividend bit. The remainder stays below 128, so it lives in A and the
   runtime's path for a remainder past 255 is gone. The first steps of a byte,
   before the remainder can reach the divisor, only shift. A 16-bit dividend
   divides its high byte first, which is no work at all when that byte is
   below the divisor (x < 1792 for 7). An 8-bit dividend by 128 or more is one
   compare. Inside a loop the steps are inline (about 70 bytes for 8 bits, 160
   for 16); elsewhere every site calls one routine per divisor and width
   (`rt.divu16_7`). A `/` and `%` of the same operands divide once: the second
   reads rt_a or rt_r while no label, call or write to them came between and
   no statement since may have written what the operands read. Signed and
   variable divisors still use the runtime, and pair the same way.
   Tests: tests/test_backend_div.cpp (every u8 dividend by every constant
   0..255 as pairs, in both orders and alone; u16 dividends over the whole
   range by 0..20 and 17 larger constants; signed, variable-divisor and pair
   edges: an operand set between, the result written into the dividend, a
   store through another pointer to the dividend, an index changed, a
   division in only one arm or in a loop that may not run, calls between,
   i/o dividends, the loop counter as dividend). The fuzz generator writes
   constant divisors (small odd and even, 128 and up, powers of two, signed)
   and `/` `%` pairs on the same operands, sometimes with a statement between
   or into the dividend itself. Changing the generator changes what each seed
   writes, so the fuzz farm's 14 programs are kept in tests/programs/fuzz_farm.

   | Measure | Before | After |
   | --- | --- | --- |
   | hi-res line of 280 pixels (new in the cycle report) | 471373 (1683.5 a pixel) | 72373 (258.5 a pixel) |
   | fill, hgr rows, sprites, multiply, life step/draw | 156411, 54922, 942, 572, 180689/46405 | the same |
   | Life generations in 10 M cycles | 43 | 43 |
   | gaps flagged: life, dot, sprites | 18.22%, 63.68%, 13.90% | the same (no division) |
   | gaps --fuzz 3000: runtime routines | 5.1% | 4.8% (the new generator) |

6. Gap list item 2, carry facts (2026-10-26). A new peephole pass
   (src/backend/carry.cpp) follows the carry along the function's control
   flow: CLC and SEC that find it already so go, and a CLC right after a
   label that every jump to the label brings clear (only falling in needs
   it) moves before the label, out of the loop. Facts come from the
   instructions (CLC, SEC, both edges of BCC and BCS, so the `CMP / BCS`
   guard of a loop) and from code generation through `AsmItem::carry`: the
   back edge of a u8 counted loop (`INX / CPX end / BNE`) leaves C clear,
   since the counter only counts up to the end (not for signed counters,
   which compare unsigned), and an add whose sum cannot pass 255 leaves C
   clear, so the next add needs no CLC. Function starts and labels named by
   jump tables or patched operands may have any carry.
   Sums come from value ranges (src/backend/ranges.cpp): a global, constant
   table or local of a byte type, or an array of them, whose every write is
   known holds no more than the largest value written. Stores through
   pointers count for every variable the pointer may reach under the
   provenance rule, followed through lets, sets, arguments, returns, and
   pointers kept in memory (arrays, struct fields, through pointers) lumped
   together; a function the program never calls gets any pointers. A null
   global pointer reaches nothing; one made with `as` only memory no
   variable uses. Life's worlds only ever hold 0 and 1, so the neighbour sum
   is `LDA (p),Y` then seven `ADC (p),Y` with one CLC before the row.
   Tests: tests/test_backend_carry.cpp (sums over an array of 0 and 1 with
   one 255 stored directly, through a pointer parameter, a returned pointer,
   a struct field, an array of pointers, a pointer to a pointer and a global
   pointer; a value just small enough and one 2 too big; a local set big in
   a branch; signed counters, the carry set before a loop, variable ends,
   break, continue, nested loops, a loop through 255, a jump table in the
   body). Every one of those fails when the fact it checks is dropped from
   the analysis. The fuzz generator has `sm`, an array of small values (0 to
   3) stored directly and through pointers and read in sums of three, and in
   a fifth of programs a store of 250 to 255 into it.

   | Measure | Before | After |
   | --- | --- | --- |
   | life step (cycle report) | 180689 (94.1 a cell) | 154005 (80.2 a cell) |
   | life draw | 46405 | 46421 (a BCC in the row multiply now crosses a page; same code) |
   | sprite step over 32 | 942 | 878 |
   | fill 8 KB | 156411 | 156409 |
   | hgr rows, multiply, hi-res line | 54922, 572, 72373 | the same |
   | Life generations in 10 M cycles | 43 | 49 |
   | gaps flagged: life, dot, sprites | 18.22%, 63.68%, 13.90% | 7.64%, 63.68%, 7.73% |

   Life's remaining CLCs are in `row_of`'s multiply by 42 (items 3 and 4).

7. Gap list item 3, strength reduction (2026-10-26). A 16-bit let at the
   top of a u8 loop's body whose value is the counter times a constant plus
   values the body never changes is worked out once before the loop and
   moved by the constant at the end of every iteration
   (src/backend/gen_reduce.cpp): `(let above (ptr u8) (row_of cur (- y 1)))`
   is now `LDA above / CLC / ADC #42 / STA above / BCC / INC above+1` per
   row instead of the inlined multiply by 42 (about 70 cycles). The value is
   followed through adds, constant multiplies and shifts, conversions,
   pointer arithmetic and calls to one-expression functions like `row_of`
   (parameters standing for the arguments). A byte part such as `(- y 1)`
   or `(<< r 1)` must not wrap for any value the counter takes, so `(- y 1)`
   from 0 or `(+ y 1)` with a variable end stays as it was. The rest must
   have no effects, not read the counter, and read no variable the body may
   write, nor memory when the body stores. The let's variable is never set
   and its address never taken, so it equals the value through its scope.
   Tests: tests/test_backend_reduce.cpp (Life's above/here/below rows, a
   byte part that wraps from 0 and at 255, a variable end, the base changed
   in the body directly, through a call and through a pointer the body
   stores through, the let set later, continue and break around it, shifts,
   a product that wraps mod 65536, a pointer minus, a byte product near
   255, nested loops); each of those checks fails when its rule is dropped.
   The fuzz generator writes such lets at the top of u8 loops (a row of wb
   through a `rowp` like `row_of`, a pointer plus the counter times a
   stride, a u16 product that may wrap), each read twice.

   | Measure | Before | After |
   | --- | --- | --- |
   | life step | 154005 (80.2 a cell) | 137331 (71.5 a cell) |
   | life draw | 46421 (24.2 a cell) | 42074 (21.9 a cell) |
   | other kernels | 156409, 54922, 878, 572, 72373 | the same |
   | Life generations in 10 M cycles | 49 | 54 |
   | gaps flagged: life, dot, sprites | 7.64%, 63.68%, 7.73% | 0.78%, 63.68%, 7.73% |

8. Gap list item 4, known-zero bytes (2026-10-26). 16-bit values keep a
   byte that is known to be 0 as the number 0 in their operand instead of
   storing it, and the code that reads them skips it. A shift left by 8 or
   more of a widened byte is the low byte moved up with a 0 below it; a
   shift whose value fits a byte by the value ranges (`(& y 7)` shifted by
   up to 5) is an 8-bit shift with a 0 above; a left shift by 5 to 7 of a
   byte is `LSR A / STA hi / LDA #0 / ROR A / STA lo` (and the like) with no
   third byte; a right shift of a byte is 8-bit; a product that fits a byte
   (`(* (as u16 (>> y 6)) 40)`) is an 8-bit multiply. Adds
   (src/backend/gen_wide.cpp) make a byte that is two numbers a number, and
   a byte that adds 0 to another byte, with no carry coming in, that other
   byte where it is: `$2000 + (y << 10)` is one byte of work. A copy of a
   byte onto itself is skipped.
   Tests: tests/test_backend_wide.cpp (every u8 through shifts by 0 to 15 of
   widened, masked, 16-bit, signed and offset values; products that just
   fit a byte and just don't; adds with constant bytes that carry, passed
   bytes, hgr.init's row address, destinations that are their own operands
   with the bytes swapped, pointers plus widened bytes). The fuzz generator
   writes 16-bit arithmetic on masked, shifted and widened bytes; it found
   that the value ranges took `(/ z n)` to be at most z, but a divisor of 0
   gives all ones (fixed, with a test).

   | Measure | Before | After |
   | --- | --- | --- |
   | hgr row tables (hgr.init) | 54922, 202 bytes | 44746, 173 bytes |
   | other kernels | 156409, 878, 572, 137331/42074, 72373 | the same |
   | Life generations in 10 M cycles | 54 | 54 |
   | gaps on hgr.init (of a 10 M-cycle dot run) | 2880 cycles at 7 sites | 2112 at 5 |
   | gaps flagged: life, dot, sprites | 0.78%, 63.68%, 7.73% | 0.78%, 63.73%, 7.73% |

9. Gap list item 5, dead loads and ALU results (2026-10-26). A peephole
   pass (src/backend/dead_regs.cpp) runs liveness of A, X, Y and the N, Z,
   C, V flags backwards over the function until it settles, and drops an
   instruction whose only effect is registers and flags that are all dead
   before their next write. Reads that may be I/O never go (numbers,
   symbols at $C000 and up, anything through a pointer), nor stores, stack
   or memory read-modify-writes; a call, a return or a jump out of the
   function reads everything. Tests: tests/test_backend_peep.cpp (unused
   I/O reads, direct, through a pointer made from a number and as u16;
   flags read after labels and calls; an unused pointer let leaves no
   load). Dropping the I/O rule or the branches' use of Z fails the tests
   and hundreds of fuzz programs.

   | Measure | Before | After |
   | --- | --- | --- |
   | kernels, Life generations | (item 4) | the same: none has a dead value |
   | 300 fuzz programs (default run) | 791262 cycles | 772479 cycles |
   | gaps --fuzz 3000: dead values | 0.65% | 0.16% |
   | gaps --fuzz 3000: compiled code flagged | 7.63% | 7.00% |

10. Gap list item 6, reloads only for flags (2026-10-26). The peephole's
    register pass knows whether N and Z show A's value (after LDA, an ALU
    op, a transfer or a shift of A, and through stores, CLC/SEC and
    branches). A load of what A already holds goes even when a branch reads
    its flags if they already show A, and is `AND #$FF` (2 cycles, the same
    flags) when they don't, as after a CMP. A byte stored from a register
    that holds another byte is known to equal that byte until either is
    written, so `STA b / ... / LDA b` with A still holding the source goes
    too. Tests: in tests/test_backend_peep.cpp, flags after `STA`, after
    `CMP` inside `and`, a copy whose source is then set, a byte written
    through a pointer between.

    | Measure | Before | After |
    | --- | --- | --- |
    | hgr row tables | 44746, 173 bytes | 43594, 169 bytes |
    | hi-res line of 280 | 72373 (258.5 a pixel) | 70553 (252.0 a pixel) |
    | other kernels, Life generations | 156409, 878, 572, 137331/42074, 54 | the same |
    | gaps flagged: life, dot, sprites | 0.78%, 63.73%, 7.73% | 0.78%, 53.95%, 7.73% |
    | gaps --fuzz 3000: loads already held | 1.22% | 1.19% |

11. Gap list item 7, inlined parameters bound to their arguments
    (2026-10-26). When a call is inlined, a parameter whose argument is a
    plain variable reads that variable where it is instead of a copy in the
    callee's frame (`Gen::bound`, followed wherever code generation looks
    through a variable: values, places, pointers, byte indices). Only when
    the parameter is never set and its address never taken, the variable is
    not a let read in its place (index aliases, forwarded lets), nothing in
    the callee or in the later arguments may change the variable, and it is
    not a loop counter held in a register (a copy in memory serves the
    callee better). hires_plot's `plot.x` and `plot.y` and Life's
    `row_of.world` lose their copies. Tests: tests/test_backend_bind.cpp
    (the callee sets the variable or its parameter, a later argument sets
    it, the callee stores through a pointer to it; pointer, index and 16-bit
    parameters, nested inlining, counters in memory); each fails when its
    rule is dropped. The fuzz generator calls small helpers that take
    pointers (`peek2`, `poke2`) with pointer variables.

    | Measure | Before | After |
    | --- | --- | --- |
    | hi-res line of 280 | 70553 (252.0 a pixel) | 65513 (234.0 a pixel) |
    | life step / draw | 137331 / 42074 | 137101 / 41909 |
    | other kernels | 156409, 43594, 878, 572 | the same |
    | Life generations in 10 M cycles | 54 | 55 |
    | gaps flagged: life, dot, sprites | 0.78%, 53.95%, 7.73% | 0.76%, 53.95%, 7.73% |

12. Gap list item 8, commuted adds (2026-10-26). A 16-bit add, and, or or
    xor whose second operand's byte the last instruction just stored from
    A (or loaded into it) works on that byte in A and reads the first
    operand instead: `STA t / LDA a / CLC / ADC t` is `CLC / ADC a`, and the
    store goes when nothing else reads t. Subtraction keeps its order.
    Tests: in tests/test_backend_wide.cpp, sums, ands, ors and xors of a
    variable and a value just computed, for every u8, and subtractions;
    commuting a subtraction fails them.

    | Measure | Before | After |
    | --- | --- | --- |
    | hgr row tables | 43594, 169 bytes | 40138, 157 bytes |
    | life step | 137101 | 136984 |
    | life draw | 41909 | 41913 (2 instructions fewer, but Life's worlds moved 40 bytes down and draw's rows cross 16 more pages) |
    | other kernels, Life generations | 156409, 878, 572, 65513, 55 | the same |
    | gaps flagged: life, dot, sprites | 0.76%, 53.95%, 7.73% | 0.76%, 53.97%, 7.73% |

13. Gap list item 9, registers kept across indexed stores (2026-10-26). An
    indexed store `STA arr,X` only changes arr (indices stay in bounds, as
    the backend assumes everywhere), so both the code generator's index
    tracker and the peephole's register pass now forget only what they knew
    about arr, instead of everything in memory. sprites' spawn writes all
    four fields of `(at sprites sprite_count)` with one `LDX sprite_count`
    instead of four. Tests: in tests/test_backend_peep.cpp, an index kept
    across stores into other arrays and a struct array, and an index that is
    an element of the array it indexes (`(at a (at a 1))`), which the store
    changes. The fuzz generator indexes byte arrays of 256 by their own
    elements.

    | Measure | Before | After |
    | --- | --- | --- |
    | sprites.spawn | 4 `LDX sprite_count` | 1 |
    | kernels, Life generations | 156409, 40138, 878, 572, 136984/41913, 65513, 55 | the same |
    | gaps flagged: sprites | 7.73% at 6 findings | 7.73% at 3 (the rest follow the data) |

14. Uncalled functions in the binary (2026-10-26). `tisple build` kept the
    code of every function, also those only inlined and those nothing calls
    (dot's compile-time-only `row_start`, 108 bytes). Now only main and what
    its code names after inlining, and theirs, are in the binary; the others
    have no code, no temporaries, and their uses no longer compete for zero
    page. Their parameters and locals stay, since inlined copies use them.
    The runtime and division routines are the ones the kept code calls. With
    no main (a library) every function stays. `BuildOptions::keep_all_functions`
    keeps them all; the tests that call functions by address set it, and the
    fuzz tests build like `tisple build` (`CompileMainText`). Tests:
    tests/test_backend_build.cpp (an inlined-only function, a chain called
    only from dead code, runtime only dead code used, a library).

    | Program | Code before | Code after |
    | --- | --- | --- |
    | life | 1680 | 1063 |
    | plasma | 1561 | 1346 |
    | snake | 1383 | 1014 |
    | dot | 580 | 395 |
    | multi | 286 | 203 |
    | keys | 263 | 160 |
    | kernels, Life generations | 156409, 40138, 878, 572, 137307/41902, 65513, 56 | the same |
    | plasma frames in 10 s | 201 | 200 (its tables moved: more indexed reads cross a page) |

15. Tables across pages (2026-10-26). Automatic placement put data at the
    first free address, so plasma's 256-byte SINE and ramp tables started
    mid-page and most `LDA RAMP_LO,Y` reads paid a cycle for the crossing;
    dropping dead code moved them and cost a frame. Now an array with no
    :align or :boundary of its own keeps each byte table (an element type
    without arrays, up to 256 entries; split tables at their own spacing)
    inside one page when some free start allows it, and later variables fill
    the gaps (src/link/layout.cpp). Tests: in tests/test_layout.cpp, a
    256-entry table after a small one, a split u16 192 and i16 200, 40- and
    48-byte tables in the gap before it, three tables of 200 that no start
    keeps in pages, and nothing overlapping.

    | Measure | Before | After |
    | --- | --- | --- |
    | plasma frames in 10 s | 200 | 205; file 2254 to 2304 bytes |
    | kernels, Life generations | 156409, 40138, 878, 572, 137307/41902, 65513, 56 | the same |
    | 300 fuzz programs (default run) | 948645 cycles | 948546 cycles |

16. Constant parts after unrolling (2026-10-26). Unrolled copies and
    macros leave `(+ (+ 5 t) 2)`, `(- (+ t 7) 1)`, `(* 1 3)`, `(set w (+ (+ w
    1) 1))` and `(store (+ (+ p 1) 2) v)`; the backend folded only trees that
    were all constant, so these were two adds, a temporary, a pointer sum per
    copy. Now src/backend/fold.cpp rewrites a copy of the program before code
    generation: chains of + and -, of *, and of &, | and ^ of one type have
    their constants combined in the type's wrapping arithmetic (check/arith,
    as the interpreter has it) after the other terms, which keep their order;
    a net negative constant is a subtract (`(- x 1)`), x * 1, x + 0, x << 0
    are x, x * 0, x & 0 and x | all-ones are the constant when x has no
    effects, a constant tree is a literal, and pointer steps of one element
    size combine (`(+ (+ p 0) 2)` is `(+ p 2)`, so `LDY #2 / STA (p),Y`). A
    single constant stays where it was written. `(at col 7)` was already an
    absolute address. Nodes nothing names any more become plain literals, so
    passes that count reads see only the live tree. Tests:
    tests/test_backend_fold.cpp (37 shapes, each for every byte: u8, i8, u16
    and i16 chains across wrapping, mixed signs, a constant first, conversions
    in between that must stop the chain, products with 0 of a call, the
    other bitwise ops, pointer chains of u8 and u16 elements in loads and
    stores, and an unroll like plasma's). Dropping the sign of a subtracted
    chain or the effects check fails them and the fuzz runs. The fuzz
    generator writes such chains (sums, differences, products, bitwise,
    identities, products with 0, chained pointer steps in loads and stores)
    in one program in eight, chosen from a stream of its own, so the other
    seeds' programs are what they were (50 of seeds 1 to 400 changed).

    | Measure | Before | After |
    | --- | --- | --- |
    | the shapes above, unrolled 3 times | 199 bytes | 124 bytes |
    | plasma's unrolled draw | 1129 bytes, 205 frames | the same (nothing to fold: indices are already addresses) |
    | kernels, Life generations | 156409, 40138, 878, 572, 137307/41902, 65513, 56 | the same |
    | 20000 fuzz programs (new generator) | 79714606 cycles | 78987892 cycles |

17. Two table reads in one operation (2026-10-26). Each of plasma's 40
    unrolled copies is `(| (at RAMP_LO (+ (at col k) top)) (at RAMP_HI (+ (at
    col k) bottom)))`: the first read went to a temporary and came back for
    the ORA (`LDA RAMP_LO,Y / STA t / ... / LDA RAMP_HI,Y / ORA t`), since
    the row counter held X. Now an 8-bit operation whose second operand is a
    table read with a computed index puts that index in X first, reads the
    first operand into A while X is held like a loop counter's register, and
    ends with `ORA RAMP_HI,X` (any of + - & | ^; for the ones that commute,
    the other way round too). The index now runs first, so neither side may
    have effects; when the first side needs X, nothing is kept. A u8 loop's
    counter may now live in memory also when its body has no loops of its
    own, which leaves both registers to the body; the cost decides, and it
    does in plasma's row loop. Tests: tests/test_backend_tables.cpp (sums,
    differences, ors, ands, xors of two reads for every byte, a read whose
    index is a read, calls in either side that change what the other side
    reads, the commuted form, and loops with the counter in memory). Without
    the effects rule they fail, and so does the fuzz run: the generator
    writes such operations, calls in the indices now and then, in one
    program in eight (from the side stream; 84 of seeds 1 to 400 now differ
    from before item 16).

    | Measure | Before | After |
    | --- | --- | --- |
    | plasma's unrolled copy | 44 cycles, 28 bytes | 38 cycles, 24 bytes |
    | plasma frames in 10 s | 205 | 231 (23.1 fps); code 1346 to 1191 bytes |
    | kernels, Life generations | 156409, 40138, 878, 572, 137307/41902, 65513, 56 | the same |
    | 20000 fuzz programs (generator with item 17's shapes) | 87088535 cycles | 86920245 cycles |

18. Strided bytes and invariant bytes in loops (2026-10-26). Plasma's
    column loop worked out `(- (* x 3) (<< t 1))` every iteration (`TXA /
    STA / ASL / CLC / ADC / STA / LDA t / ASL / STA / LDA / SEC / SBC`), and
    the row loop `(+ (* y 5) (<< t 1))`: the gap finder's top site (strided
    values, 6.7% of the run). Item 7 only reduced 16-bit lets. Now
    (src/backend/gen_reduce_byte.cpp) a byte expression anywhere in a u8 or
    i8 loop's body that is the counter times a constant plus things the body
    never changes, through +, -, constant multiplies and shifts and u8/i8
    conversions, is worked out before the loop into a temporary and stepped
    at the end of every iteration (`INC`/`DEC` for 1 and -1); with no counter
    in it, it is only worked out once (hoisted). Byte arithmetic wraps, so the
    step is exact for any values and the counter's range does not matter.
    Code reading the expression reads the temporary. Only when its estimated
    code costs more than reading and stepping, and only what is safe to read
    before a loop that may not run: no loads through pointers, no arrays at
    fixed addresses. Tests: in tests/test_backend_reduce.cpp, plasma's sums,
    steps that wrap over all 256 counter values, -1 and 0 steps, an i8
    counter from below 0, an invariant the body sets directly, through a
    call and through a pointer, continue and break, a loop that may not run,
    nested loops; treating everything as invariant fails them and the fuzz
    run (the generator already writes such loops).

    | Measure | Before | After |
    | --- | --- | --- |
    | plasma's column and row loops | 31 and 27 cycles of the sum each time | a load and an ADC #3 / #5 |
    | plasma frames in 10 s | 231 | 243 (24.3 fps); code 1191 to 1203 bytes |
    | kernels, Life generations | 156409, 40138, 878, 572, 137307/41902, 65513, 56 | the same |
    | 20000 fuzz programs | 86920245 cycles | 86336946 cycles |

19. Dead stores to globals (2026-10-26). The dead-store pass only knew a
    function's own locals and temporaries. It now also takes the locals of
    the functions a function inlines (only code that writes them first
    reads them), its own parameters, and globals and other functions'
    parameters, which every call (JSR, jsr included), jump out of the
    function and return reads. Pointers never reach these (provenance: they
    are not address-taken); address-taken variables, explicit addresses
    (maybe I/O) and bytes read indexed anywhere in the function stay as they
    were (src/backend/dead_stores.cpp, StoreScope). Tests: in
    tests/test_backend_peep.cpp, overwritten globals, a call or the return
    in between on every path, loops, an address-taken global behind a
    pointer, a fixed address, a callee's parameter; without the call rule or
    the return rule they fail, and so do the fuzz run and the paint program
    (tests/test_machine_mouse.cpp).

    | Measure | Before | After |
    | --- | --- | --- |
    | 20000 fuzz programs | 86336946 cycles | 86163802 cycles |
    | gaps --fuzz 3000: dead stores | 161200 cycles (1.29%) | 159688 (1.28%): the rest are the programs' own sets on some paths |
    | kernels, Life, plasma | 156409, 40138, 878, 572, 137307/41902, 65513, 56, 243 | the same |

20. Registers after a label (2026-10-26). The peephole's register pass
    forgot everything at every label. A label that only forward branches and
    JMPs of the same function name (no jump table, patched operand, JMP
    (ind) or the entry) now starts with what the fall into it and every one
    of those jumps agree on: registers, constants and copies in memory, and
    whether N and Z show A (src/backend/peephole.cpp, Meet). Tests: in
    tests/test_backend_peep.cpp, ifs, matches and breaks whose arms agree or
    not; taking the fall's state alone fails them and about 350 of 20000
    fuzz programs.

    | Measure | Before | After |
    | --- | --- | --- |
    | life step / draw | 137307 / 41902 | 137300 / 41888 |
    | 20000 fuzz programs | 86163802 cycles | 86156137 cycles |
    | other kernels, Life generations, plasma | 156409, 40138, 878, 572, 65513, 56, 243 | the same |

## Gap finder (2026-10-26)

`tisple gaps prog.tsp` finds where the output is worse than it could be,
ranked by the cycles a run spends there (src/gaps, src/superopt).

- **Profiler.** It runs the program on the IIe (`--sim`: the bare simulator;
  no main: every function in turn) for `--cycles N` (10 M by default). For
  each instruction it notes when the condition held: the carry was already
  set or clear, a load or transfer left the register as it was, a store left
  memory as it was, a store was overwritten before anything read it, a
  branch always went one way, an instruction's register and flags were never
  read, or a stored byte stepped by the same amount every time. A site is
  reported when the condition held on every one of 16 or more executions.
  These are hints that some fact would make the instruction redundant, not
  proofs. Cycles saved are what this run would have saved.
- **Superoptimizer** (`--superopt`). It takes straight-line windows of 2 to
  `--window N` (6) instructions at the 30 hottest starts and works out what
  is live after each one. Then it finds the cheapest sequence, by cycles and
  then bytes, with the same effect on everything live. Candidates use the
  documented opcodes with the window's own operands and 0, 1, $FF and the
  window's constants. Each result is checked against random vectors, on
  every input when the inputs are 24 bits or fewer (marked "proved",
  otherwise "tested"), and on the cycle-exact core. A second search takes
  what the profile saw on entry as facts (the carry, register and operand
  ranges) and names the facts it needs. `--stochastic STEPS` (MCMC) handles
  longer windows; its results are only tested.
- **Bulk.** `tisple gaps --fuzz N --seed S --threads T` runs the fuzz
  generator's programs and adds them up by pattern and shape. Fixed-code
  runtime routines are counted separately. The totals don't depend on T.
  `--print` prints a seed's program.

**Reach.** The exhaustive search finishes on windows of up to 6
instructions: lengths 2 to 4 in about 0.1 s at most, length 5 in 0.1 s
median (6 s at worst), and length 6 in about 4 s. On Life's inner loop
(three `(zp),Y` operands, A live, 21 cycles), 10 of 15 length-6 windows
finish within 30 M nodes. No 7-instruction window (23 to 26 cycles) finishes
within 30 M nodes (about 8 s). The default budget of 1 M nodes (about 0.3 s)
finishes 169 of Life's 186 searches. A whole run takes 8 s on Life and 17 s
on dot. 3000 fuzz programs take 27 s on 8 threads.

### Findings

Cycles are per 10 M-cycle run unless noted.

- **life** (18.2% of the run flagged). The 7 CLCs in step's neighbour sum
  are redundant on every execution: 1.16 M cycles, 11.6% of the run. The
  superoptimizer proves the sum without them (21 to 15 cycles per three
  terms), given that the carry is clear and the cells are 0..1. The row
  pointers step by 42 (strided `y*42`, about 0.5 M, 5%). Each inlined
  `row_of` copies `cur` into `row_of.world`, which already holds it. The
  multiply's `ROL` of the high byte does nothing while y < 64. Two exact
  windows: `ASL A / ROL $1B / STA $1A / LDA $37 / CLC / ADC $1A` becomes
  `ASL A / STA $1A / ROL $1B / CLC / ADC $37` (an add of what A holds, done
  the other way round: 3 cycles a row, 4 times), and `TXA / STA $39`
  becomes `STX $39`.
- **dot, hgr.init** (one run of about 55 K cycles). It does 16-bit
  arithmetic on 3-bit values: high bytes stored as 0 and added back, 4
  CLCs after shifts that can't carry, and a `BCC` over an `INC` that never
  runs. Exact windows save 8 cycles a row: `STA $18 / LDA #0 / STA $17 /
  LSR A / ROR $18` becomes `LSR A / STA $18 / LDA #0 / STA $17`, plus the
  commuted add. Given the known-zero high byte, `ROL $1B / ASL A / ROL $1B /
  ASL A / ROL $1B` is just `ASL A / ASL A`. About 13 K cycles of the 55 K
  could go. The idle loop of main reloads `action` for its flags (`STA $0A /
  LDA $0A` becomes `AND #$FF / STA $0A`, proved).
- **sprites** (no main; functions called in turn). The CLC in
  step_sprites always finds the carry clear (668 K, 6.7%). The loop's
  `CPX count / BNE` back edge and the `CMP count / BCS` guard both leave C
  clear in the body. spawn reloads `LDX sprite_count` 3 more times, because
  the indexed stores in between make the backend forget X. The other
  step_sprites findings (`BEQ` never taken, the guard never taken) follow
  the data.
- **hires_plot** (library). `(/ x 7)` and `(% x 7)` each call divu16 on the
  same operands, about 750 cycles each, for about 1665 cycles a pixel. One
  call gives both results. The 16-bit divisor 7 is also passed again each
  time.
- **fuzz, seeds 1..3000** (9.2 M cycles). In compiled code: dead stores
  1.3% (mostly the programs' own repeated sets), stores of what memory held
  1.0%, strided values 1.0%, loads of what a register held 1.0% (`STA zp /
  LDA zp` for flags is the top shape), and dead values 0.7% (`LDA lo / LDA
  hi` reloaded after a pointer is formed and never used). Runtime routines
  add 5.1%, almost all divu8/divu16 paths for divisors of 128 and up, which
  never run. Exact superoptimizer windows add 0.3%: dead reloads, `LDA m /
  CLC / ADC m` as `LDA m / ASL A`, `STA m / LDA n / LDX m` as `TAX / STA m
  / LDA n`, and self-copies `LDA (p),Y / STA (p),Y`.

### Fixes the data argues for, by cycles

Items 1 to 9 are done (Fixed 5 to 13); what is left of them:

7. The let chains in macros.shuffle (18 to 6 cycles, proved): `(let x u8
   a)` copies a parameter the function then changes through x; seeing that
   the copy and the swap just rename bytes needs value numbering across
   lets, not inlining. (Inlined parameters are done, see Fixed 11.)
9. What registers hold after a label: `label ; LDA zp` in fuzz runs (0.10%)
   needs the register tracker to merge what every way into the label
   brings. (Indexed stores are done, see Fixed 13; `TAX ; LDA abs,X` in
   fuzz runs is values that happen to equal their index.)

### After the fixes (2026-10-26)

All nine items, from the gap finder's merge to the last of them:

| Measure | Before | After |
| --- | --- | --- |
| fill 8 KB | 156411 (19.1 a byte) | 156409 (19.1 a byte) |
| hgr row tables (hgr.init) | 54922 | 40138 |
| sprite step over 32 | 942 | 878 |
| 16-bit multiply | 572, worst 773 | 572, worst 773 |
| life step | 180689 (94.1 a cell) | 136984 (71.3 a cell) |
| life draw | 46405 (24.2 a cell) | 41913 (21.8 a cell) |
| hi-res line of 280 | 471373 (1683.5 a pixel) | 65513 (234.0 a pixel) |
| Life generations in 10 M cycles | 43 | 55 |
| gaps flagged: life | 18.22% | 0.76% |
| gaps flagged: dot (its idle loop) | 63.68% | 53.97% |
| gaps flagged: sprites | 13.90% | 7.73% |

The new top of each report:

- **life** (0.76%): the row lets' steps themselves (`STA bottom`, strided
  by -88 or +42: already the add), loads of `here` and `below` that happen
  to equal what A holds (`here` is `above` + 42), and the remaining CLCs of
  the multiply by 42 that sets the rows up once per generation.
- **dot** (53.97%): the idle loop with no key pressed (a compare that never
  branches, the key stored and overwritten, `action` stored as it was);
  these follow the data.
- **sprites** (7.73%): `(if (at sprites i active) ...)` never false and the
  loop guard never taken, both following the data.
- **fuzz, seeds 1..3000** (6.0% of compiled code, with the new generator):
  strided values 1.8% (mostly u16 loops' own counters and pointers), dead
  stores 1.3% (the programs' own repeated sets), stores of what memory held
  1.1%, loads of what a register held 1.1% (`label ; LDA zp` the top shape
  left), dead values 0.17%.

## Benchmarks

| Program | Measure | Result | Date |
| --- | --- | --- | --- |
| life | generations in 10 M cycles (about 10 s on a IIe) | 10 | 2026-10-26 |
| life | the same, byte indices as (p),Y (item 4) | 43 | 2026-10-26 |
| life | the same, gap finder items 1 to 9 (Fixed 5 to 13) | 55 | 2026-10-26 |
| hires_plot | cycles a pixel of a 280-pixel line | 1683.5 -> 234.0 | 2026-10-26 |

