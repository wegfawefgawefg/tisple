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

1. Divide once for a `/` and `%` pair on the same operands, and specialise
   division by a constant: no 16-bit divisor, and no high-divisor path for
   divisors under 128. This is about 45% of hires_plot's hline, and most of
   the 5% that fuzz runs spend in the runtime.
2. Carry facts. A counted loop's exit test (`CPX/CPY/CMP` then `BNE`, or
   the `BCS` guard) leaves C clear in the body, so the first CLC there can
   go: 6.7% of sprites, and the first of Life's 7. For the other 6, value
   ranges for arrays whose every store is a small constant (world_a and
   world_b only ever hold 0 and 1) show the sum can't carry: 10% of Life.
3. Strength-reduce `(* y STRIDE)` on a loop counter, including through an
   inlined `row_of`. Keep the row pointer and add the stride each time:
   about 5% of Life.
4. Known-zero bytes in 16-bit arithmetic on widened bytes. Don't store,
   shift or add high bytes that are known to be 0 (`(<< (as u16 (& y 7))
   10)`, `y*42` while y < 64). This is most of hgr.init's 24% and about 1%
   of Life.
5. A register-liveness pass in the peephole: drop loads and ALU results
   whose register and flags are both dead before the next write. This is
   0.7% of fuzz runs and the most common exact superoptimizer result.
6. Reloads made only for flags. When the flags already follow A, as after
   an LDA or ALU op, drop them. Otherwise test the flags another way
   (`AND #$FF`, 2 cycles). Top load shape in fuzz runs; dot's main loop.
7. When inlining, bind parameters that are plain variables to those
   variables instead of copying them in: Life's `row_of.world`,
   hires_plot's `plot.x` and `plot.y`, and the let chains in macros.shuffle
   (18 to 6 cycles, proved).
8. Commuted adds. For `(+ a b)` with b just computed in A, add a to it
   instead of storing b and reloading a: 3 cycles each, proved, in Life's
   and hgr.init's row arithmetic.
9. Keep what registers hold across indexed stores that can't reach zero
   page (`STA abs,X` with abs >= $100): sprites' spawn, and fuzz shapes
   `TAX ; LDA abs,X` and `label ; LDA zp`.

## Benchmarks

| Program | Measure | Result | Date |
| --- | --- | --- | --- |
| life | generations in 10 M cycles (about 10 s on a IIe) | 10 | 2026-10-26 |
| life | the same, byte indices as (p),Y (item 4) | 43 | 2026-10-26 |

