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

21. Race item 1, step pointers for u16 counters and while loops (2026-10-27).
    The race (docs/races.md) showed `str + n` rebuilt with a 16-bit add every
    byte (strlen), sieve's k and i in memory with their pointers rebuilt, and
    search's `(addr_of (at text i))` worked out and copied every position.
    Now:
    - While loops keep variables in registers (src/backend/gen_while.cpp): a
      u16 local in Y and memory like a u16 for counter, with every array and
      pointer it indexes read through a derived pointer `base + (high << 8)`
      set before the loop; its sets move the pointers' high bytes (INY / BNE /
      INC, or worked out again), the code for that put in once the body is
      done and the pointers are known. Up to two byte locals in X and Y the
      same way (INX, DEY, TAX after a value). The loop's sets write the
      register, which is stored back where the loop ends. As for u8 for
      loops, each choice is generated and the cheapest kept; one whose code
      touches its registers or calls anything is dropped. With the step of 1
      last, the test goes to the top, so the step's BNE goes straight back.
    - Facts (src/backend/gen_facts.cpp): in the second half of an and, an
      if's arm and a while body, what the condition says about a u8 variable
      holds until a statement that may change it starts, so `(at arr (- j
      1))` after `(!= j 0)` is `arr-1,X` (sort).
    - Step pointers (src/backend/gen_step.cpp): a u16 local a loop only steps
      by constants, and a u16 for counter kept in memory, index their arrays
      and pointers through `ptr = base + v`, set before the outermost such
      loop and moved wherever v is stepped (sieve's outer loop, mandel's k).
      A u16 for counter now goes to Y or memory by cost, not Y whenever it
      can, and lets of its body that move with it (search's p) are stepped.
    - Smaller: a let pointer is worked out straight into its variable; a
      byte of an `:address` symbol is known, so a page-aligned base adds no
      `ADC #$00`; a memory u16 counter ending on a page skips the low-byte
      compare; CMP of a pinned register is CPX or CPY, and the peephole turns
      `TXA / CMP` into `CPX` when A is not read after; a load whose flags
      only flow through a label is still dropped when it is redundant.
    Tests: tests/test_backend_while.cpp (wide while loops up and down across
    pages, steps by 1, 3, 255, 256, 300 and by variables, other sets, steps in
    arms and not last, break, continue, return, calls that are and are not
    inlined, nested loops; byte while loops; facts with sets in arms, in
    while bodies and in loops in arms; step pointers through pointers, in
    nested loops, both ways; let pointers whose index reads them). Leaving out
    the fixups, the step pointers' moves or the facts' ends fails them. The
    fuzz generator writes such loops (tests/fuzz_gen_loops.cpp) in one program
    in four, from a stream of its own; dropping the fixups or the moves fails
    3000 fuzz programs, and dropping the facts' ends 4 of 20000.

    | Measure | Before | After |
    | --- | --- | --- |
    | race strlen | 108102 | 36181 (12 cycles a byte; cc65 36153) |
    | race sieve | 1455915 | 1105991 (Oscar64 1258060) |
    | race search | 317417 | 199654 |
    | race sort | 717510 | 474565 |
    | race mandel | 9149633 | 9141874 |
    | other race kernels | 95624, 86293, 115266, 65930, 540509, 160650, 705567, 82295 | the same |
    | 20000 fuzz programs (generator before this item) | 86156137 cycles | 83941902 cycles |
    | kernels, Life generations, plasma | 156409, 40138, 878, 572, 137300/41888, 65513, 56, 243 | the same |

22. Race item 1, the rest: page pointers (2026-10-27). line's plot reads
    `(at COL_BYTE x)` and `(at COL_BIT x)` with a u16 x that no loop steps,
    and built `table + x` with a 16-bit add for each. Now a byte of a table at
    a u16 index in memory is (ptr),Y with Y the index's low byte: the
    pointer's low byte is the table's, stored once where the function starts,
    and only its high byte is worked out, the table's plus the index's (LDA /
    CLC / ADC / STA, 10 cycles instead of 20). Each place in the code has a
    pointer of its own, four a function at most, since one statement may read
    the same table at another index while a store's pointer is waiting. The
    peephole's register pass now also keeps what it knows across a label
    nothing names (an inlined call's return label), so plot's second `LDY x`
    goes. Tests: in tests/test_backend_index.cpp, a table starting mid-page
    at indices around every page edge, three places of one table in one
    statement, a store whose value reads another index of the same table
    (one pointer for all places fails it), inlined copies.

    | Measure | Before | After |
    | --- | --- | --- |
    | race line | 705567 | 664333 |
    | race life | 160650 | 160648 |
    | other race kernels | (as after 21) | the same |
    | life step / draw (cycle report) | 137300 / 41888 | 137389 / 41912 (2 TXA for LDA, but the code is 2 bytes shorter, so Life's worlds start 2 bytes lower and more rows cross pages) |
    | Life generations, plasma | 56, 243 | the same |

23. Race item 2, multiply (2026-10-27). rt.mul16 shifted the whole 16-bit
    multiplier right and the multiplicand left, both in memory, and tested
    the multiplier with LDA/ORA every bit; two zero-extended bytes went to it
    too. Now (src/backend/runtime.cpp) the product's low 16 bits are split by
    bytes: a0 * b0 is the whole product of the low bytes, the textbook
    shift-and-add unrolled (the multiplier's bits leave the bottom of one byte
    as the product's low byte comes in at the top, the high byte builds up in
    A: 12 cycles a bit), and a1 * b0 + a0 * b1 only reaches the high byte, so
    each is a byte multiply kept to 8 bits that ends when its multiplier runs
    out and is skipped for a zero high byte. Two operands whose high bytes are
    known zero, or whose values stay below 256, call the new rt.mul8x8 (the
    whole 16-bit product of two bytes); with one byte, it goes second, where
    rt.mul16 skips its zero high byte. Tests: tests/test_backend_mul.cpp (two
    widened bytes for every pair, byte times word both ways round, words
    with zero high bytes only at run time, signed operands); calling the byte
    routine for a value up to $FFF fails it.

    | Measure | Before | After |
    | --- | --- | --- |
    | race mul8 | 95624 | 43774 (Oscar64 63165) |
    | race mul16 | 86293 | 63443 (Oscar64 65477) |
    | race mandel | 9141874 | 7185582 (Oscar64 7601337) |
    | 16-bit multiply 1234*5678, $FFFF*$FFFF | 572, 773 | 419, 537; 66 to 159 bytes with the routine |
    | other race kernels, Life generations, plasma | (as after 22) | the same |

24. Race item 2, quarter squares (2026-10-27). A multiply inside a loop
    (the code generator's loop depth, inlined calls included) now calls a
    quarter-square routine: x * y = sq(x + y) - sq(|x - y|) for bytes, with
    sq(n) = n * n / 4 rounded down from a 1 KB table (low and high bytes for
    n = 0 to 255, then 256 to 511), which is exact since x + y and x - y are
    both odd or both even. rt.mul8x8q is about 60 cycles for the whole 16-bit
    product of two bytes (shift and add: about 120), rt.mul8q the low byte
    only, and rt.mul16q splits words as rt.mul16 does, its byte multiplies by
    the table (about 150 cycles against 420). Multiplies outside loops keep
    the shift-and-add routines, and a program has the table only when one of
    its loops multiplies: none of programs/ and examples/ does (their sizes
    are unchanged or smaller). The tables are a span of their own, "runtime
    tables", data rather than code. Decision on the cost: 1 KB of data for a
    loop that multiplies is worth it on the race (mul8 -33%, mul16 -51%,
    mandel -40%); a program that cannot spare it can multiply outside its
    loop or by shifts. Tests: in tests/test_backend_mul.cpp, every pair of
    bytes widened and kept to 8 bits, in and outside loops, word pairs and
    every byte against each word, both ways round, in and outside loops; every
    one of the six routines is in the test program.

    | Measure | Before | After |
    | --- | --- | --- |
    | race mul8 | 43774 | 29331 |
    | race mul16 | 63443 | 31296 |
    | race mandel | 7185582 | 4323385 |
    | programs/ and examples/ sizes | (as after 23) | the same, no tables |
    | other race kernels, Life generations, plasma | (as after 23) | the same |

25. Race item 3, registers for loop variables (2026-10-27). Item 21 put while
    loops' variables in registers, but sort's inner loop could only have Y:
    the outer for kept i in X, so v stayed in memory (`LDA v / CMP arr-1,Y`
    every pass). Now a while loop with no return in it may take the register
    an outer u8 for loop keeps its counter in: the counter is stored to its
    variable before the loop (it never changes inside, so the loop's reads of
    it read that) and loaded back where the loop ends. And the code weights
    the choices compare by are no longer 4 per loop level: a for loop with
    constant bounds weighs its trip count (2 to 64), a while loop 8, other
    loops 4 (Gen::loop_factors), so an inner loop that runs 16000 times
    against its outer loop's 255 wins the registers. Tests: in
    tests/test_backend_while.cpp, the sort with the outer counter read
    inside the while and after it, a while inside two for loops taking both
    registers, a while with a return (which must not take them); leaving out
    the load back fails 5 of them and 32 of 3000 fuzz programs.

    | Measure | Before | After |
    | --- | --- | --- |
    | race sort | 474565 | 460950 (24 cycles a pass, Oscar64 22) |
    | other race kernels | (as after 24) | the same |
    | 3000 fuzz programs before item 21's generator (against the base backend) | 10448034 | 9190469 cycles (-12%, most of it multiplies); 1574 faster, 138 slower (multiplies by small numbers, data moved by the tables) |
    | Life generations, plasma | 56, 243 | the same |

26. Race item 4, reusing what a register holds (2026-10-27). sprites stored
    the new x with `STA $4000,Y` and loaded it twice more for its two
    compares; the peephole's register pass only knew plain bytes. It now also
    knows indexed ones (src/backend/peephole.cpp): after a load or store of
    abs,X, abs,Y or (zp),Y the register holds that byte as long as the index
    register keeps its value, nothing stores into that array (an indexed or
    plain store of the same symbol, an INC, a store through a pointer), and,
    for (zp),Y, nothing is stored at all and the pointer's bytes stay. A
    store of what the byte already holds goes too (crc16 stored crc's low
    byte back unchanged). Tests: in tests/test_backend_peep.cpp, the sprites
    loop, stores to other elements of the same array, an index changed, a
    store through a pointer into the array, struct fields of one array, an
    INC through X while the stored byte is reached through Y, (zp),Y across a
    store through another pointer and a pointer change. Never forgetting on
    an index change fails 146 checks and 5 of 3000 fuzz programs; keeping
    indexed bytes across stores into their array fails the INC test, and 6 of
    10000 fuzz programs with a new loop shape (a store, an INC of the same
    array, a reread).

    | Measure | Before | After |
    | --- | --- | --- |
    | race sprites | 82295 | 65959 (Oscar64 76125) |
    | race crc16 | 540509 | 534365 |
    | other race kernels, Life generations, plasma | (as after 25) | the same |

27. Race item 5, constant offsets in addresses (2026-10-27). races/kernels'
    fill/unroll.tsp and memcpy/unroll.tsp write `(at buf (+ (* page 256) (as
    u16 i)))` for 32 pages: the index was a 16-bit sum, so every store built
    a pointer. Now (src/backend/gen_place.cpp, PeelConstant) the constants of
    a u16 index, `(+ x c)`, `(+ c x)`, `(- x c)` and chains of them, move into
    the element's address: an index stays inside its array, so x + c never
    wraps. The rest of the index is a widened byte here, so each store is
    `STA buf+page*256,X`: 5 cycles a byte for fill, 9 for a copy. (`(at arr (-
    j 1))` with a u8 j, sort's, became `arr-1,X` in item 21 through facts.)
    Tests: in tests/test_backend_index.cpp, byte, split u16 and struct arrays
    at constants around a parameter, a byte, a u16 counter in Y and a stepped
    variable; adding a subtracted constant fails them, and 3 of 3000 fuzz
    programs with new loop shapes (constants around u16 counters and stepped
    variables, inside wb and ww).

    | Measure | Before | After |
    | --- | --- | --- |
    | race fill (unroll.tsp; the loop kernel.tsp) | 115266 (kernel.tsp) | 43022 (cc65 63996); kernel.tsp the same |
    | race memcpy (unroll.tsp) | 65930 (kernel.tsp) | 38151 (cc65 59878); kernel.tsp the same |
    | other race kernels, Life generations, plasma | (as after 26) | the same |
    | life step / draw (cycle report) | 137389 / 41912 | 139086 / 42832: the code is 34 bytes shorter (main drops stores of held values since item 26), so the worlds start 34 bytes lower and more rows cross pages; step itself is the same code |

28. Race item 6, 16-bit logic and compares by byte (2026-10-27). crc16's
    bit loop worked `(& crc 0x8000)` out into two temporaries and ORed them
    to test it, then each arm shifted crc into temporaries and copied them
    back. Now:
    - `(& x c)` against 0 tests only the bytes c has bits in, a load and an
      AND each, and for the top bit alone the load's N flag (`LDA x+1 /
      BMI`); the and is never a value (src/backend/gen_cond.cpp, MaskTest).
    - A signed 16-bit compare with a constant goes high byte first: the high
      bytes decide unless equal (compared unsigned with their top bits
      flipped, or by the sign alone against 0), and only then the low bytes
      do. mandel's guard is four of them.
    - A variable shifted into itself by a constant shifts in place (`ASL x /
      ROL x+1` for one or two steps) instead of through a temporary.
    - The CRC step, `(if (!= (& v top) 0) (set v (^ (<< v 1) k)) (set v (<< v
      1)))` and the like (either test, either arm order, more in an arm, u8
      or u16), shifts v once in place before the branch: the bit tested falls
      into the carry, and both arms read v for their (<< v 1). Only when the
      arms read v nowhere else and call nothing (src/backend/gen_shift.cpp,
      GenShiftedIf).
    Tests: tests/test_backend_bytewise.cpp (masks of single bits, bytes and
    both, both ways round and from memory, u8 and u16; signed compares with
    18 constants around 0, the byte edges and the ends, all four operators;
    shifts in place by 1 to 15, unsigned and signed; CRC steps for u16 and u8,
    either test, an arm reading the variable outside the shift). Comparing
    high bytes unflipped, jumping out of a two-byte mask test after its first
    byte, or branching the wrong way on the carry each fail them, and the
    last two fail 106 and 19 of 3000 fuzz programs with a new loop shape (CRC
    steps on a local and masks of its bits).

    | Measure | Before | After |
    | --- | --- | --- |
    | race crc16 | 534365 | 244665 (Oscar64 221527) |
    | race mandel | 4323385 | 4201007 |
    | race line | 664333 | 664201 |
    | other race kernels, Life generations, plasma | (as after 27) | the same |

29. Race item 8, loop control (2026-10-27). Loops paid for their control:
    crc16's `(for k u8 0 8 ...)` counted up and compared (`INX / CPX #8 /
    BNE`) although nothing read k, while loops jumped to their test at the
    bottom, and search's inner loop, entered with j = 0, tested `j < 6` first
    anyway. Now:
    - A u8 for loop whose body never reads its counter and whose bounds are
      constants counts down to 0 from its trip count (`DEX / BNE`, or `DEC /
      BNE` in memory).
    - A while loop whose test is small (24 expressions, no calls) tests once
      before the loop instead of jumping to the test at the bottom; the
      bottom test then follows the body straight on (its label only stays for
      a continue), so `DEX / BEQ` needs no `CPX #0` (sort).
    - The peephole knows the flags after a compare or immediate load of
      known values and decides a branch on them: gone when never taken, a JMP
      when always (search's first pass: `CPY #6 / BCS` with Y = 0).
    - `LDA #k / TAY` with A unread after is `LDY #k` (and the like).
    Tests: in tests/test_backend_while.cpp, counted-down loops of 1, 2, 255
    and 256 trips, with break and continue, nested and in memory; rotated
    whiles that never run, with continue, and/or tests and registers in the
    test; loops entered with known counters on both sides of their bound.
    One trip more fails 25 checks; a carry wrong for equal values fails one.

    | Measure | Before | After |
    | --- | --- | --- |
    | race crc16 | 244665 | 228281 |
    | race sort | 460950 | 423514 (Oscar64 422725) |
    | race search | 199654 | 169880 |
    | race sieve | 1105991 | 1102807 |
    | race mandel | 4201007 | 4177022 |
    | race sprites | 65959 | 65957 |
    | other race kernels, Life generations, plasma | (as after 28) | the same |
    | 3000 fuzz programs before item 21's generator (against the base backend) | 10449220 | 9109808 cycles (-12.8%); 1712 faster, 139 slower |

30. Race item 7, unrolling (2026-10-27). A u8 for loop whose body never
    reads its counter, with up to 16 constant trips and at most 160
    expressions in all the copies, runs as that many copies of its body, one
    after the other: no counter, no test (a continue goes to the next copy).
    crc16's 8 bits are 8 copies of the CRC step. And a wide while loop (item
    21) that steps its u16 variable by one and reads it nowhere but through
    its derived pointers, strlen's, no longer keeps the high byte up to date:
    it is worked out once after the loop from the pointer (`LDA ptr+1 / SEC /
    SBC #>base / STA high`), which saves an INC a page. Tests: in
    tests/test_backend_while.cpp, unrolled loops with break and continue in
    the copies and loops inside them (a continue that breaks fails them); the
    strlen shapes check the high byte (subtracting the base's low byte fails
    3 of them, and 27 of 3000 fuzz programs with a new shape, a u16 that is
    only an index stepped by one).

    | Measure | Before | After |
    | --- | --- | --- |
    | race crc16 | 228281 | 188348 (Oscar64 221527); 75 to 202 bytes |
    | race strlen | 36181 | 36136 (cc65 36153) |
    | other race kernels, Life generations, plasma | (as after 29) | the same |
    | programs/ sizes | snake 1011 code bytes, multi 203 | snake 1024, multi 205 (unrolled loops) |

31. Race item 8 and 10, search (2026-10-27). Three changes. Jump threading
    in the peephole: a branch or JMP to a JMP, or to a branch whose outcome
    the flags already decide, goes straight to where that one goes (only
    where the flags are dead after the target, so a skipped compare is not
    needed). Loop setup (the counter's start, the reduced lets' first values,
    the high byte of a wide loop) now counts at the weight outside the loop,
    not inside it, so a choice that does more work once to save work every
    time can win. And a u16 for counter in memory can be replaced by a
    reduced let of its body stepped by one (gen_counter.cpp): search's
    `(let p (ptr u8) (addr_of (at text i)))` keeps p - i in a temporary
    worked out once, the loop stops when p reaches that plus the end, and
    the body's reads of i (search's `(set result i)` on a match) are p minus
    the difference. The counter's INC and compare go from the loop; WordLoop
    tries it as a third choice next to the wide loop and the memory loop and
    keeps the cheapest. Tests: in tests/test_backend_while.cpp, the race's
    search kernel with the needle at the start, at page edges, at the end and
    nowhere (checking two INCs are left), and replaced counters read as a
    value, as another array's index, inside a loop, after a continue and from
    a start past the end; a fuzz shape for the search (a u16 for with a
    pointer to wb that moves with it, a copy or a sum of the counter on a
    match, now and then a break). Making the difference one too big fails 20
    of 3000 fuzz programs.

    | Measure | Before | After |
    | --- | --- | --- |
    | race search | 169880 | 156790 (jump threading), 132297 (replaced counter; LLVM-MOS 136618) |
    | other race kernels, Life generations, plasma | (as after 30) | the same |
    | programs/ sizes | snake 1024 code bytes, multi 205 | the same |

32. Race items 4 and 9, sort (2026-10-27). Two changes. The peephole's
    register pass now knows what registers hold at the top of a loop: a scan
    of a copy of the function, knowing nothing at loop labels, notes what
    holds at each jump back to one; the real scan then meets that with what
    falls in. The facts at those jumps held without assuming anything at
    the label, and every change the scan makes keeps what registers and
    memory hold, so they hold in the changed code too (N and Z showing A is
    not carried over). The offsets of indexed bytes wrap now, so arr-1,Y
    (arr+65535) is tracked. And a while test's compare e > v (or v < e) of
    a u8 variable the loop never sets can be done as e >= t, with t = v + 1
    worked out before the loop; v = 255 never compares greater, and the
    compare is one of the test's and-ed parts, so then the loop does not
    run (the carry of the add goes to the loop's end). It is one more set of
    while choices (gen_while.cpp), with v out of the registers; it wins when
    the body starts by loading the byte it compared, which the peephole then
    drops at the loop top (the choice counts that, TopLoadSaved). Insertion
    sort's inner loop was TXA / CMP arr-1,Y / BCC / LDA arr-1,Y / STA arr,Y /
    DEY / BEQ; it is now LDA arr-1,Y / CMP t / BCS / STA arr,Y / DEY / BEQ, and
    X keeps the outer counter. Tests: in tests/test_backend_while.cpp, the
    four compare forms (<= and >= are not bumped: e <= 255 always holds, and
    the first fuzz run with the new shape caught that the loop then skipped)
    with v = 255 and 0, the compare first and last, a test too big to copy,
    v an outer counter or a let, break and continue, an insertion sort; a fuzz
    shape (BoundWhile: such a while, the bound often 255 or 0, the body's
    first value the compared byte). Without the skip for 255, 16 of 3000 fuzz
    programs and 5 tests fail; with t worked out from e instead of v, 1 and 5;
    a loop label taking the jumps' state without meeting the fall-in, 176 and
    32.

    | Measure | Before | After |
    | --- | --- | --- |
    | race sort | 423514 | 356260 (Oscar64 422725); 47 to 51 bytes |
    | other race kernels, Life generations, plasma | (as after 31) | the same |
    | programs/ sizes | snake 1024 code bytes, multi 205 | the same |

33. Race items 10, 6 and 9, line (2026-10-27). The race draws 3149 pixels
    (2777 x steps, 1679 y steps); Oscar64 took 183 cycles a pixel, Tisple 211.
    Four changes. An inlined call's parameter now reads a conversion of the
    caller's variable in place when the conversion is only its bytes (as u16
    of an i16, as u8 of a u16, as u16 of a u8): plot's (as u16 x) and (as u8
    y) were copied into plot's frame for every pixel (18 cycles). A 16-bit
    shift left by one into another variable goes through A (LDA / ASL A /
    STA / LDA / ROL A / STA) instead of copying and shifting in memory. A
    local that every let and set gives 1 or -1 (unit_steps.cpp: Bresenham's
    step_x and step_y) steps a variable by INC or DEC, its sign picking which
    (LDA step+1 / BMI / INC x / BNE), instead of a 16-bit add: 13 to 17
    cycles instead of 20. And a 16-bit compare whose left side's high byte A
    still holds (the let just worked out) compares the high bytes first:
    they decide unless equal (signed: of the same sign as unsigned, of
    different signs the other way round, EOR showing which), then the low
    bytes, unsigned. Tests: tests/test_backend_steps.cpp, the compares
    signed and unsigned, every operator and both ways round, over pairs of
    edge values; unit steps of every size crossing pages and wrapping, either
    way round, in a while register, and a local that is also 2; plot-like
    callees bound to converted variables, a sign extension (still copied).
    A fuzz shape (StepWhile: a unit step moving a variable, a 16-bit let of
    it compared at once). Mutations: the signed differ branch flipped fails
    7 of 2000 fuzz programs and 6 tests, the unsigned one 46 and 8, BMI for
    BPL on the step's sign 33 and 5, any set counting as a unit 68 and 37,
    ASL for the ROL 5 and 8, the conversions not bound 1 test (a count).

    | Measure | Before | After |
    | --- | --- | --- |
    | race line | 664201 | 607519 (bound conversions), 598144 (shift), 574401 (unit steps), 569746 (high bytes first; Oscar64 576133); 373 to 375 bytes |
    | other race kernels, Life generations, plasma | (as after 32) | the same |
    | programs/ sizes | snake 1024 code bytes, multi 205 | the same |

34. Race item 7, scans two at a time (2026-10-27). A wide while loop (item
    21) whose body is only its variable's step by one, strlen's, runs two
    tests and steps a time: Y is even at the top, so the first INY never
    leaves the page and needs no branch; only the second does (INY / BNE).
    An odd Y starts at the second test (TYA / LSR A / BCS, once). The copies
    count at half the loop's weight, since each runs half as often (without
    that, the choices took a memory loop instead, twice as slow). strlen's
    loop is 21 cycles for two bytes instead of 24. Tests: in
    tests/test_backend_while.cpp, scans of t (whose zeros start pages, so
    every scan crosses one) from odd and even starts, at an offset; a fuzz
    shape (ScanWhile: a u16 from odd and even starts stepped by one while a
    byte of wb is not some value, up to a limit across pages). Without the
    odd start, 18 of 2000 fuzz programs and 2 tests fail; on the wrong
    parity, 39 and 2.

    | Measure | Before | After |
    | --- | --- | --- |
    | race strlen | 36136 | 31642 (cc65 36153); 45 to 54 bytes |
    | other race kernels, Life generations, plasma | (as after 33) | the same |
    | programs/ sizes | snake 1024 code bytes, multi 205 | the same |

35. Race item 10, search's constants (2026-10-27). A replaced counter (31)
    whose loop has a constant start and end and whose let is then a
    constant at the start (an array's address plus the start) has its
    difference and stop worked out while compiling: the let starts as LDA
    #<text / STA, the loop's end test compares with numbers (CMP #, 2 cycles
    instead of 3), the counter's memory is never set, and a read of the
    counter subtracts a number. For that an index that is a variable bound
    to a constant reads as the constant (gen_place.cpp). Tests: in
    tests/test_backend_while.cpp, replaced counters with constant bounds, a
    start past 0, an offset into the array, read as a value, on a match and
    after a break. The first version left the let reading the counter's
    memory, never set any more; the search kernel test caught it. With the
    difference off by the start, 7 of 2000 fuzz programs and 2 tests fail;
    the stop one too far, 10 and 1; the lets read the stale counter, 7 and 3.

    | Measure | Before | After |
    | --- | --- | --- |
    | race search | 132297 | 128734 (LLVM-MOS 136618); 107 to 74 bytes |
    | other race kernels, Life generations, plasma | (as after 34) | the same |
    | programs/ sizes | snake 1024 code bytes, multi 205 | the same |

36. Race item 3, page pointers that follow (2026-10-27). line's plot reads
    COL_BYTE and COL_BIT at x through page pointers (22), and set both
    pointers' high bytes at every pixel (18 cycles), though x only crosses a
    page a few times a line. Now a while loop finds the u16 variables it only
    steps by one (gen_follow.cpp: (+ v 1), (- v 1), or a unit step, item 33),
    and page pointers indexed by them keep their high bytes: set once before
    the loop, moved by INC or DEC right where a step carries into the
    variable's high byte. The steps are generated before every pointer is
    known, so they note their labels and the pointer code goes in when the
    loop is done; a set of the variable some other way (none so far) would
    work the high bytes out again there. Loops with loops inside do not
    follow (an inner loop's choices could keep the variable in a register).
    Tests: in tests/test_backend_steps.cpp, loops with a call in them (so the
    variable stays in memory) stepping up and down across pages, by unit
    steps and by constants, two tables, reads before and after the step, a
    break, no trips, a variable also stepped by 3; the tables differ on
    every page (the first data repeated every 512 bytes and hid a wrong
    page). The StepWhile fuzz shape now also indexes wb with a u16 it steps,
    reading and writing there. Mutations: INC for DEC on the way down fails
    7 of 3000 fuzz programs and 2 tests; the steps not noted, 9 and 3; no
    high bytes before the loop, 24 and 3.

    | Measure | Before | After |
    | --- | --- | --- |
    | race line | 569746 | 507296 (Oscar64 576133); 375 to 383 bytes |
    | other race kernels, Life generations, plasma | (as after 35) | the same |
    | programs/ sizes | snake 1024 code bytes, multi 205 | the same |

37. Race item 3, invariant loads out of loops (2026-10-27). With what the
    jumps back to a loop label bring known (32), a load right after the
    label whose value every jump back already holds, at a label only the
    fall and those jumps reach, moves before the label and runs once (the
    flags it sets must be unread). sieve's first loop stored `LDA #1` every
    byte. And a u16 for loop with its counter's low byte in Y tests the end
    at a page step with X when X is free (`LDX i+1 / CPX #hi`), so A keeps
    what the body left there. (fill's `LDA v` stays: the store through the
    loop pointer could, for all the peephole knows, change v.) Tests: the
    fuzz programs; hoisting without checking the jumps back fails 476 of
    3000 and 213 tests, without checking the flags 6 and 2.

    | Measure | Before | After |
    | --- | --- | --- |
    | race sieve | 1102807 | 1086425 (Oscar64 1258060) |
    | life step (test) | 139025 | 138894 |
    | other race kernels, Life generations, plasma | (as after 36) | the same |
    | programs/ sizes | snake 1024 code bytes, multi 205 | snake 1021, multi 203 |

38. Fuzz farm run 6: loops entered by a jump (2026-10-29). The farm found 227
    miscompiled programs in about 33 million (1 in 150,000). All 227 have one
    cause, in the peephole's register pass: what holds at a loop label (32)
    met the fall and the jumps back, but not the jumps from before the label
    into it, which it never noted (`arriving` only kept mergeable labels).
    Such a jump comes from a loop body that starts with an if on a test the
    code generator decides, `(if (> g 255) (continue))` or `(<= g3 127)` on an
    i8: the test is a JMP over the arm, the flow pass sends the branch back
    through that JMP, and so the JMP, reached from the loop's entry, is a way
    into the loop's label. The scan, falling there from nothing, took only
    what the jumps back brought: `(set g 94)` in the loop was dropped as a
    store of what g held. 168 of the programs fail from 18d16cf (32) on; the
    other 59 from 61cd7f2 (37), whose hoist checked that no jump from before
    the label had arrived, but none ever had, so a load moved before the
    label ran only on a fall nothing reached, not on the jump in.
    Now (src/backend/peephole.cpp) a jump to a loop label from before it is
    noted like one to a mergeable label, and the label meets it too; the
    hoist then sees it and stays put. The stopped attempt (005af54) also kept
    jump threading to targets after the jump, but threading back is sound:
    the target's state already met the way in from the compare's branch,
    whose label met the jump being threaded, and the farm's programs, 12
    million new ones from the generator and 2 million with the new shape
    below pass with it (at the farm's rate, 80 would have failed before). Tests: in tests/test_backend_peep.cpp, a
    loop starting with a continue that never runs and storing a constant, the
    farm's hoisted load, and a branch back threaded past a test to a label
    the branch in reaches; the 227 programs as they were written are in
    tests/programs/fuzz_farm/run6 (seeds.txt lists each seed and how it
    failed). Leaving out the jumps ahead fails all 227, the 3 tests, and 1636
    of 80000 seeds' programs with the new shape; the hoist without its
    check, 51 of them, the hoist test and 19.
    Why local fuzzing missed it: the plain generator writes such a loop only
    by chance (a compare of a variable with its type's own bound, at the start
    of a loop body, with something carried round the loop), about 1 in 150,000
    programs, and local runs are 300 to 20000; mutation checks for 32 and 37
    were run on those programs, where no loop was entered by a jump. The
    generator now writes that shape (tests/fuzz_gen_entries.cpp, EntryLoop:
    for loops of each counter type and range and a while, whose body starts
    with an if on a test that never holds, as a statement or a value, then
    constant stores, stores back, loads and stores at an index the loop
    never moves, inner loops, ifs with equal arms) from a stream of its own,
    in a second program for one seed in four (`seed N (entries)`), so
    GenerateProgram's programs stay what they were (the seeds 1-300 hash
    holds). Without the fix 8% of those programs fail. The fuzz report now
    prints a line for every failing seed, not only the first three.

    | Measure | Before | After |
    | --- | --- | --- |
    | race kernels | mul8 29331, mul16 31296, fill 43022, memcpy 38151, sieve 1086425, strlen 31642, search 128734, sort 356260, crc16 188348, mandel 4177022, life 160648, line 507296, sprites 65957 | the same |
    | kernels (tisple_tests) | fill 156409, hgr rows 40138, sprites 878, multiply 419/537, life 138894/42920, line 65513 | the same |
    | Life generations, plasma frames in 10 s | 56, 243 | the same |
    | programs/ and examples/ binaries | 13 | the same, byte for byte |
    | 20000 fuzz programs (seeds 1-20000, the plain generator) | 166379613 cycles | 166379631 |

39. Exact delays and string literals (2026-10-29, docs/language.md): text_demo
    prints its names and status with `text80.put_str`, 1179 bytes less code.
40. Mid-level passes 1 to 3 (2026-10-29, docs/midlevel-optimizer.md, src/opt):
    effect summaries, dead lets, stores and statements, and deletion of loops
    that provably end with no effect outside them. Race seed 21258: 264845
    cycles to 50 (Oscar64 19). Random race, seeds 1-2000: geomean against the
    fastest C compiler 3.087 to 2.420, wins 6.1% to 9.0%, worst 1427x to 312x;
    1421 programs faster, 66 slower by at most 1.5% (data placed elsewhere once
    code shrinks). Kernels, Life 56, plasma 243 and the tisple_tests cycle lines
    the same; every program and example byte for byte except snake and
    examples/speaker, whose empty delay loops (lib/speaker `buzz`, `tone`) are
    now deleted: they need `(wait_cycles n)` (docs/language.md). 20000 fuzz
    seeds: 208691712 cycles to 177542577. Tests: tests/test_opt*.cpp; fuzz
    shape tests/fuzz_gen_dead.cpp (`seed N (dead)`, one seed in four).
    At merge, lib/speaker `buzz` and examples/speaker `tone` wait with
    `wait_cycles` (half now in cycles), same pitch: 180 and 980 cycles per half
    wave in snake, 822 in the example.

41. Mid-level passes 4 and 5 (2026-10-29, docs/midlevel-optimizer.md):
    constant propagation across statements (src/opt/consts*.cpp) and
    loop-invariant code motion (src/opt/licm*.cpp). Random race, seeds
    1-2000, against the fastest C compiler: geomean 2.420 to 2.124 (consts
    alone 2.213, licm alone 2.321), median 2.00 to 1.74, wins 9.0% to 11.7%,
    worst 312x to 289x (seed 1241); the division family (889 programs)
    geomean 3.00 to 2.52, median 2.47 to 2.15. 1188 programs faster, 128
    slower (81 by under 2%, the worst 16%: loops left on their first pass
    after their invariant work moved). Kernels, Life 56, plasma 243 and the
    tisple_tests cycle lines the same; every program and example byte for byte
    but text_demo (code 4587 to 4574 bytes). 20000 fuzz seeds (plain, entries,
    dead): 232958305 cycles to 201174190. Pass time on text_demo 0.2 to 0.8 ms.
    Dead locals keep their frame bytes (the interpreter shares the compiled
    layout). Tests: tests/test_opt_consts.cpp, test_opt_licm.cpp; fuzz shape
    tests/fuzz_gen_motion.cpp (`seed N (motion)`, one seed in four).
    At merge with string literals: consts folded const-table reads, the link
    then left those tables out, and the interpreter (running the program as
    written on the compiled layout) read other data there: 50 fuzz mismatches,
    none in the 6502 code. The link now keeps every table the program as
    written names (FindNamedTables). text_demo's built chapter got 18 lines so
    the primes keep 1.6 KB of margin for the language card.

42. Mid-level passes 6 and 7 (2026-10-29, docs/midlevel-optimizer.md): common
    subexpressions (src/opt/cse*.cpp), facts from conditions
    (consts_facts.cpp) and calls of pure functions worked out at compile time
    (eval.cpp; 20000 interpreter steps a call, 400000 a compile). Random race,
    seeds 1-2000, against the fastest C compiler: geomean 2.124 to 2.111 (cse
    off 2.113, eval off 2.112: nearly all of it is facts from conditions),
    median 1.74 to 1.73, wins 11.7% to 11.8%, worst 288.84 (seed 1241) the
    same; the division family geomean 2.52 to 2.50, median 2.15 to 2.13.
    102 programs faster, 21 slower (seed 495 the worst, 6%: an inner match a
    fact cut leaves a loop that licm then guards for too little; the rest data
    and zero page placed elsewhere). The race's programs seldom work
    a value out twice or call a pure function with constants (11 of 400 have a
    repeated product or division in one function), so cse and eval are for
    real programs. Kernels, Life 56, plasma 243 and the tisple_tests cycle
    lines the same; every program and example byte for byte but text_demo
    (code 3395 to 3376 bytes: `(last_top)` worked out once in next_chapter).
    20000 fuzz seeds (the variants both run): 326822489 to 325445564 cycles
    (2290 faster, 685 slower, 516 of those by under 1%); the two worst, 7086
    (86 to 273) and 12904 (motion), are backend choices a fact changes: byte
    work hoisted out of a loop from a clause that never runs
    (gen_reduce_byte.cpp collects from every arm), and a pointer kept out of
    zero page. TISPLE_FUZZ=30000 from four bases: 269638 programs, 0
    mismatches. Pass time on text_demo 0.6 to 0.75 ms. Tests:
    tests/test_opt_cse.cpp, test_opt_eval.cpp, facts in test_opt_consts.cpp;
    fuzz shape tests/fuzz_gen_common.cpp (`seed N (common)`, one seed in four).
    Found on the way: the link read its list of the tables the program as
    written names past its end once the passes add variables (ASan).
43. Unrolled nests (2026-10-29, gen_loop.cpp): each for checked its own
    copies against the unroll budget and counted a loop inside it once, so a
    nest of short loops unrolled level by level; with the dead work around
    them gone, a 3 KB fuzz farm program (run 10, seed 7000731954 dead) took
    47 KB. A loop now counts the copies the unrolled loops around it make.
    That program: 1576 bytes of code (1818 at -O0). Every program, example,
    kernel and race program is unchanged. Test in test_backend_while.cpp.
44. Threaded jumps back into loops (2026-10-29, peephole.cpp RegisterPass):
    a jump to a test the registers decide may go straight to where the test
    sends it, and that can be back to a loop label. The first scan recorded
    what its jumps back to each loop label bring before it threaded or removed
    them: a branch it then removed counted, a jump it threaded there did not.
    The second scan used those facts at the label and threaded the same jump,
    so the loop body believed A held the 0 of the removed way while the
    threaded way brought 1 (farm run 10, seed 6001453322 dead: a `let` of 0
    done as TAY). Now the first scan records the jumps as they end up, the
    second threads back to a loop label only when every fact there holds at
    the jump (Implies), and never back to a label no jump goes back to. Every
    binary, kernel and race program is unchanged (text_demo's print_number
    keeps its threaded loop). Farm programs 6001453322.tsp and _reduced.tsp.
    TISPLE_FUZZ_OPT adds pass options to TISPLE_FUZZ_FILE (-O0 showed this was
    the backend).
45. Backend division and multiplies (2026-10-29): divisions by a variable
    take one compare when the dividend is below the divisor or the divisor is
    0, 1 or has its top bit set, else one step per quotient bit
    (runtime_div.cpp); signed constant divisors divide magnitudes in a routine
    per divisor; constant multiplies use signed digits, every byte product
    inline; quarter squares for every multiply once one may run twice
    (run_counts.cpp); pointers bound in inlined calls live in zero page. Random
    race seeds 1-2000: geomean 2.124 to 1.659, median 1.74 to 1.42, wins 11.7%
    to 23.4%; divide family 2.53 to 1.51, multiply 2.01 to 1.79. Kernels, Life,
    plasma the same; snake, hires_plot, text_demo change (bigger divu8/divu16).
46. Mid-level slice 4 (2026-10-29, docs/midlevel-optimizer.md): loop collapse
    (src/opt/collapse*.cpp), value ranges (ranges*.cpp), unread arguments
    (args.cpp); licm without its quarter-square rule, its constant multiply
    costs as gen_mulconst.cpp emits them. The worst 40 race losses (base
    c6cffa8, geomean 1.649) were llvm-mos deleting loops whose passes all do
    the same or whose last pass decides (seed 1658: a 240-pass loop storing
    one constant at one place), adding up counters in closed form (g2 += 1 six
    times a pass), and deciding compares and matches from ranges (a match on
    a counter in 1..5 with clauses 15, 28, 66; 65535 >= anything); loops kept
    alive by an argument nobody reads. Random race, seeds 1-2000, against the
    fastest C compiler: geomean 1.649 to 1.433, median 1.41 to 1.28, wins
    23.6% to 28.8%, worst 110.34x to 99.99x (seed 76); against llvm-mos
    1.477 to 1.283, Oscar64 1.041 to 0.903. One at a time off: collapse 1.605,
    ranges 1.461, args 1.453; the licm change alone 1.649 (3 programs faster,
    1 slower). 863 programs faster, 63 slower (the worst 17%, most under 5%):
    data or zero page moved, and inlined calls storing the literal 0 of an
    unread argument where they bound a variable before (seed 1695, 64 to 75).
    Also seen: once a collapsed loop leaves one multiply that runs once, the
    backend's run counts pick the shift-and-add routine (seed 1997: 1528 to
    1462, 1260 when quarter squares were kept). Seed 1559 12800
    to 795 cycles, 1658 9324 to 206, 50 22344 to 732, 920 4399 to 93. Kernels,
    Life 56, plasma 243 and the tisple_tests cycle lines the same; every
    program and example byte for byte (text_demo's disk too) but
    examples/macros (61 to 63 bytes: `(times 3 ((inc x) (inc y)))` adds 3
    once, 32 to 20 cycles). Fuzz: 6 bases x TISPLE_FUZZ=30000 (450,000
    programs) 0 mismatches, 6 x 700 under ASan 0. One mismatch on the way
    (seed 5016237, collapse): a pass set a global from the counter and called
    a function that indexes by it; calls and loads now count what they read
    as varying (test in test_opt_collapse.cpp). Pass time on text_demo 0.74
    to 0.79 ms. Tests: tests/test_opt_collapse.cpp, test_opt_ranges.cpp; fuzz
    shape tests/fuzz_gen_collapse.cpp (`seed N (collapse)`, one seed in four).

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

