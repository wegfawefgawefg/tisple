# Compiler races

Started 2026-10-26. Tisple against the best 6502 C compilers, cc65 V2.19, LLVM-MOS
(llvm-mos-sdk v23.4.0) and Oscar64 v1.32.273, on 13 kernels. Every binary runs in our own
cycle-exact simulator, so all four are on one clock. `races/README.md` has the versions,
download URLs, flags and the timing rules; `races/run.sh` reruns everything and prints the
tables below. Add a dated note when something changes; regenerate the tables with
`races/run.sh --update-doc`. Ten whole programs (a parser, Dhrystone, a decompressor, a
bytecode interpreter, ...) are raced the same way in "Program race"; the same race on
thousands of random programs is the last section, "Random programs".

In short: the kernel is `main` in every language. Its inputs are written into fixed
buffers when main is entered, so no compiler can fold them, and the cycles count from
main's first instruction to its RTS. Every output is checked against a Python reference.
Each C compiler is tried with several speed settings and two ways of declaring the
buffers, and the C side also gets its library (`memset`, `memcpy`, `strlen`) as an
alternative to the loop. The fastest correct build is the score. Bytes are all the code
the kernel ran, library routines included.

## Results

Cycles / bytes, 2026-10-27, after the backend work on the ranked list below (dogfood.md
Fixed 21 to 36). The first run (2026-10-26, on `a6dc31e`) is in the repository's
history; its ratios head the "Before" sections below. Ratio is Tisple's cycles over the
fastest other compiler's; below 1 Tisple wins. No kernel source changed, in either
language.

<!-- race results: begin (races/run.sh --update-doc writes this part) -->

Cycles / bytes, best setting of each compiler:

| Kernel | cc65 | llvm-mos | oscar64 | tisple | Winner | Tisple / best other |
| --- | ---: | ---: | ---: | ---: | --- | ---: |
| mul8 | 80903 / 184 | 101512 / 114 | 63165 / 113 | 29331 / 111 | tisple | 0.46 |
| mul16 | 101818 / 299 | 90156 / 130 | 65477 / 131 | 31296 / 222 | tisple | 0.48 |
| fill | 63996 / 165 | 238896 / 58 | 90467 / 57 | 43022 / 109 | tisple | 0.67 |
| memcpy | 59878 / 143 | 153449 / 75 | 65824 / 69 | 38151 / 102 | tisple | 0.64 |
| sieve | 2285731 / 210 | 2510574 / 250 | 1258060 / 128 | 1086425 / 154 | tisple | 0.86 |
| strlen | 36153 / 39 | 186081 / 75 | 39103 / 36 | 31642 / 54 | tisple | 0.88 |
| search | 536420 / 145 | 136618 / 142 | 150500 / 63 | 128734 / 74 | tisple | 0.94 |
| sort | 2020054 / 122 | 463713 / 56 | 422725 / 47 | 356260 / 51 | tisple | 0.84 |
| crc16 | 650111 / 218 | 257916 / 253 | 221527 / 78 | 188348 / 202 | tisple | 0.85 |
| mandel | 11216044 / 656 | 9969128 / 582 | 7601337 / 422 | 4177022 / 553 | tisple | 0.55 |
| life | 1333565 / 689 | 232244 / 163 | 238566 / 163 | 161089 / 340 | tisple | 0.69 |
| line | 2451456 / 805 | 767813 / 405 | 576133 / 335 | 506033 / 360 | tisple | 0.88 |
| sprites | 819546 / 424 | 117496 / 119 | 76125 / 85 | 65957 / 78 | tisple | 0.87 |
| geomean, x the fastest | 3.30x | 2.31x | 1.46x | 1.00x | | 0.720 |

### mul8: 8-bit multiply: 256 byte products summed in 16 bits

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 80903 | 184 | -Oirs -Cl const |
| llvm-mos | 101512 | 114 | -O2 const |
| oscar64 | 63165 | 113 | -O2 const |
| tisple | 29331 | 111 | - |

Winner: tisple. Tisple / best other: 0.46.

### mul16: 16-bit multiply: 128 word products summed, wrapping

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 101818 | 299 | -Oirs -Cl const |
| llvm-mos | 90156 | 130 | -O2 const |
| oscar64 | 65477 | 131 | -O2 striped |
| tisple | 31296 | 222 | - |

Winner: tisple. Tisple / best other: 0.48.

### fill: Fill 8 KB with a byte

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 63996 | 165 | -Oirs -Cl const lib.c |
| llvm-mos | 238896 | 58 | -O2 const |
| oscar64 | 90467 | 57 | -O2 const lib.c |
| tisple | 43022 | 109 | unroll.tsp |

Winner: tisple. Tisple / best other: 0.67.

### memcpy: Copy 4 KB

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 59878 | 143 | -Oirs -Cl const lib.c |
| llvm-mos | 153449 | 75 | -O3 const |
| oscar64 | 65824 | 69 | -O2 const lib.c |
| tisple | 38151 | 102 | unroll.tsp |

Winner: tisple. Tisple / best other: 0.64.

### sieve: Sieve of Eratosthenes, 8192 flags, count the primes

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 2285731 | 210 | -Oirs -Cl const |
| llvm-mos | 2510574 | 250 | -O2 extern |
| oscar64 | 1258060 | 128 | -O2 const |
| tisple | 1086425 | 154 | - |

Winner: tisple. Tisple / best other: 0.86.

### strlen: Length of a 3000-byte string

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 36153 | 39 | -Oirs -Cl const lib.c |
| llvm-mos | 186081 | 75 | -O2 const |
| oscar64 | 39103 | 36 | -O2 const |
| tisple | 31642 | 54 | - |

Winner: tisple. Tisple / best other: 0.88.

### search: Naive search for 6 bytes in 4 KB of text (found at 3500)

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 536420 | 145 | -Oirs -Cl const |
| llvm-mos | 136618 | 142 | -O3 extern |
| oscar64 | 150500 | 63 | -O2 const |
| tisple | 128734 | 74 | - |

Winner: tisple. Tisple / best other: 0.94.

### sort: Insertion sort of 256 bytes

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 2020054 | 122 | -Oirs -Cl const |
| llvm-mos | 463713 | 56 | -O2 const |
| oscar64 | 422725 | 47 | -O2 const |
| tisple | 356260 | 51 | - |

Winner: tisple. Tisple / best other: 0.84.

### crc16: CRC-16-CCITT, bit by bit, over 1 KB

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 650111 | 218 | -Oirs -Cl const |
| llvm-mos | 257916 | 253 | -O3 const |
| oscar64 | 221527 | 78 | -O2 const |
| tisple | 188348 | 202 | - |

Winner: tisple. Tisple / best other: 0.85.

### mandel: Mandelbrot, 32 x 24 pixels, 16-bit fixed point (6 fraction bits)

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 11216044 | 656 | -Oirs -Cl const |
| llvm-mos | 9969128 | 582 | -O2 extern |
| oscar64 | 7601337 | 422 | -O2 const |
| tisple | 4177022 | 553 | - |

Winner: tisple. Tisple / best other: 0.55.

### life: One Life step, 40 x 48 cells, as programs/life

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 1333565 | 689 | -Oirs -Cl const |
| llvm-mos | 232244 | 163 | -O2 extern |
| oscar64 | 238566 | 163 | -O2 const |
| tisple | 161089 | 340 | - |

Winner: tisple. Tisple / best other: 0.69.

### line: 24 hi-res lines (Bresenham and table plot, as lib/hgr)

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 2451456 | 805 | -Oirs -Cl const |
| llvm-mos | 767813 | 405 | -O2 const |
| oscar64 | 576133 | 335 | -O2 striped |
| tisple | 506033 | 360 | - |

Winner: tisple. Tisple / best other: 0.88.

### sprites: Move and bounce 64 sprites (array of structs) for 16 frames

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 819546 | 424 | -Oirs -Cl extern |
| llvm-mos | 117496 | 119 | -O2 extern |
| oscar64 | 76125 | 85 | -O2 striped index.c |
| tisple | 65957 | 78 | - |

Winner: tisple. Tisple / best other: 0.87.

<!-- race results: end -->

## 2026-10-27: Tisple wins all 13

After the ranked list's items (each in docs/dogfood.md, Fixed 21 to 36, with its tests),
Tisple is fastest on every kernel, by 6% (search) to 2.2x (mul8, mul16). Per kernel, what
did it, and the slack left:

- **mul8 0.46, mul16 0.48, mandel 0.55.** Multiplies by bytes (an 8x8 for zero-extended
  bytes, `rt.mul16` a byte of the multiplier at a time, skipping zero high bytes), and
  inside loops quarter-square tables (1 KB, about 40 cycles for 8x8). mandel's guard
  compares are high byte first. Slack: mandel's |z| < 128 fits a signed 8x8 the backend
  does not narrow to (item 9).
- **fill 0.67, memcpy 0.64** (the `unroll.tsp` versions): constant offsets fold into
  `STA buf+C,X`, so the page-split loops are what cc65's library is, unrolled further. The
  plain `kernel.tsp` loops are as before (fill still reloads its value each byte).
- **sieve 0.86.** The inner while keeps k's low byte in Y and indexes through a pointer on
  its high byte; the outer `flags[i]` is a pointer stepped with i; the first loop's `LDA
  #1` runs once. Slack: the inner loop still works the pointer's high byte out from k
  every pass (39 cycles a pass).
- **strlen 0.88.** n's low byte in Y, two tests and steps a time (Y even at the top).
- **search 0.94.** The u16 counter is replaced by the text pointer itself (stepped by INC,
  compared with a constant stop), j in Y, the rotated inner test, jump threading past the
  repeated `j = 6` test. Slack: LLVM-MOS's unrolled 6-byte compare with the needle in
  zero page; the first byte's `LDY #0 / LDA (p),Y / CMP needle` could be a zero-page
  compare.
- **sort 0.84.** j in Y, v + 1 worked out once so the compare leaves arr[j-1] in A for the
  move, the peephole knowing what A holds at the loop top: 19 cycles a pass against
  Oscar64's 21. Slack: v's spare `LDA v` per element.
- **crc16 0.85.** The bit test is `BMI`, the shift is in place before the branch, the 8
  bits are 8 unrolled copies.
- **life 0.69.** As before (row pointers, eight pre-offset pointers). Slack: the 0/1 sums'
  `CLC`s (gaps: 23K).
- **line 0.88.** plot's arguments read x and y in place, step_x and step_y (only 1 or -1)
  step by INC or DEC, the error compares are high byte first, and the COL_BYTE and COL_BIT
  page pointers follow x (high bytes set once and moved when x crosses a page). Slack: y
  stays an i16 (Oscar64 narrows it to X; item 9), and the loop is too long for short
  branches back (`BPL *+5 / JMP`).
- **sprites 0.87.** The stored field stays in A for the two compares.

The ranked list below is kept as it was written. Items 1 to 8 and 10 are done as
described; item 9 is done only in part (facts from conditions, compares against v + 1,
locals that are only 1 or -1), not as narrowing.

## Before: where Tisple won and lost (2026-10-26)

Tisple wins **life** (0.69) and is within 10% on **memcpy** (1.10) and **sprites** (1.08).
It loses the rest, by 1.2x to 3x. Oscar64 is the compiler to beat: it wins 8 of 13. cc65
wins only through its hand-written assembly library (memset, memcpy, strlen); its compiled
loops are 4x to 11x slower than the best (sort, life, line, sprites), except where a
runtime routine does the work (its 8x8 multiply puts it second on mul8). LLVM-MOS wins
search by unrolling, but its C library (memset 29 cycles a byte, strlen 62) loses every
memory kernel.

### life: Tisple wins (160650 against LLVM-MOS's 232244)

The row pointers are stepped by 42 once a row instead of recomputed, every neighbour gets
its own pre-offset loop pointer so all eight loads are `ADC (ptr),Y` with Y = x and no Y
reloads, and the rule is a compare chain on A. LLVM-MOS uses one pointer and reloads Y with
eight constant offsets a cell (`LDY #$2A / ADC (rc6),Y`); Oscar64 is similar. Tisple still
has slack: seven `CLC`s a cell that a sum of 0/1 values never needs (gaps: 23K cycles), and
the four row pointers are first set with four inline x42 multiplies (the 403 bytes).

### mul8 (1.51, Oscar64) and mul16 (1.32, Oscar64)

The loops are alike; the multiply routine is the difference. Tisple has one runtime
`rt.mul16`: 16x16, both operands in zero page, shifting the 16-bit multiplier right and
testing it with `LDA/ORA` every bit, the product in memory. For mul8 it is called with two
zero-extended bytes. Oscar64 narrows that case to `mul16by8` (multiplier in A, the loop
ends when A runs out of bits, product low byte in Y). Its `mul16` takes the multiplier a
byte at a time with a sentinel bit (`SEC / ROR`, then `LSR` until zero), skips the high
byte when it is zero, and keeps the product's low byte in Y. cc65 (80903 on mul8) also
has a dedicated 8x8 routine. None of them uses a squares table; that is the bar hand
assembly sets (about 40 cycles for 8x8 to 16).

### fill (1.80, cc65) and memcpy (1.10, cc65)

cc65 wins with its assembly `memset`/`memcpy`: two pointers, `STA (ptr1),Y / STA (ptr2),Y
/ INY` unrolled twice (7.8 cycles a byte for fill, 14.6 for copy). Tisple's loops are
already the page-loop shape (`STA (ptr),Y / INY / BNE`, the high byte bumped per page) and
beat Oscar64's and LLVM-MOS's compiled loops, but fill reloads the loop-invariant value
every byte (`LDA v`: 14 cycles a byte, could be 11). The fastest shape for a fixed-address
buffer is page-split: `STA $2000,X / STA $2100,X / ...` 32 stores per X (5.2 cycles a
byte). Tisple can say that already with `macros.unroll` (`fill/unroll.tsp`,
`memcpy/unroll.tsp`), but `(at buf (+ (* page 256) (as u16 i)))` compiles to a pointer
built per store instead of `STA buf+page*256,X`, so the unrolled versions are 2.6x slower
than the loops today. Folded, they would be about 42K and 38K cycles, ahead of cc65.

### sieve (1.20, Oscar64)

The inner loop `(set (at flags k) 0) (set k (+ k i))` costs Tisple 54 cycles a pass: k
lives in memory, the pointer `flags + k` is rebuilt every pass with a 16-bit add (its low
half is `ADC #$00`: the buffer is page-aligned), then k += i in memory and k's high byte
is loaded again for the test. Oscar64 (46 cycles) keeps k's low byte in X, adds only the
high byte to the buffer's page, and steps the low byte in a register. The outer loop is
the same story with i: Oscar64 steps a pointer next to i. Stepping the pointer itself by i
would be about 33 cycles a pass. `tisple gaps` points at exactly this: 46.7% of the run is
"strided value" stores.

### strlen (2.99, cc65)

Tisple rebuilds `str + n` with a 16-bit add every byte (36 cycles a byte). Oscar64's loop
(the compiled C loop, not a library) keeps the pointer's low byte at 0 and uses Y as n's
low byte: `INY / BNE / LDA (ptr),Y / BNE`, with the high byte of n in X bumped with the
pointer on a wrap, 13 cycles a byte (39103). cc65's assembly `strlen` does the same
(36153). gaps: 50% strided value.

### search (2.32, LLVM-MOS)

LLVM-MOS fully unrolls the 6-byte inner compare, holds the six needle bytes in zero page
registers, steps the text pointer with `INC` and keeps i in X and Y. Tisple rebuilds
`(addr_of (at text i))` each position (a u16 `for` counter is not reduced the way u8 ones
are), copies it to `p` through a temporary, keeps j in memory, enters the inner loop with a
`JMP` to its test, and after the loop tests `j = 6` again although the exit already told.
gaps finds nothing here (0%): the costs are structural.

### sort (1.70, Oscar64)

Inner loop 22 cycles a pass in Oscar64 against 40 in Tisple. Oscar64 keeps j in X and v in
Y, reads `arr[j-1]` as `CMP $3FFF,X` (the -1 folded into the address), and `DEX / BNE` is
both the step and the `j != 0` test. Tisple keeps j in memory (`LDY j / DEY / LDA arr,Y /
INY / STA arr,Y / DEC j / LDA j / BEQ`, then `LDA v / LDY j / DEY / CMP arr,Y` for the
test). gaps finds nothing: register allocation and offset folding are not redundancies.

### crc16 (2.44, Oscar64)

Per bit, Tisple computes `(& crc 0x8000)` into two temporaries, ORs them to test, then
evaluates `(<< crc 1)` into temporaries and copies back, in each arm. Oscar64 tests the bit
with `BIT crc+1 / BPL`, shifts crc in place (`ASL / ROL`), applies the polynomial as two
`EOR #imm`, and counts the 8 bits down in X. Tisple also stores crc's low byte back
unchanged after `crc ^= byte << 8` (gaps: "store already held").

### mandel (1.20, Oscar64)

Three `rt.mul16` calls an iteration dominate both. Oscar64's multiply is the faster one
(see mul16): for positive values below 128 its high-byte pass ends at once, while
Tisple's 16-bit shift loop runs until the multiplier is zero, all 16 bits for a negative
one. The range guard (`|zx| < 128`) means every multiply here fits a signed 8x8 multiply,
which nobody exploits. Tisple's four signed compares against constants are each
`CMP / SBC / BVC / EOR / BMI`; Oscar64 checks the high byte first and only then the low.

### line (1.22, Oscar64)

`(at COL_BYTE x)` and `(at COL_BIT x)` with u16 x index 280-byte tables: Tisple builds a
16-bit pointer `table + x` for each (twice a plot; gaps: 22.5% of the run as strided
values), plus copies of x and y into the inlined plot's frame. Oscar64 sets the two table
pointers' low bytes once outside the loop and per plot adds only x's high byte to the
table's page, indexing with Y = x's low byte. It also narrows lib/hgr's i16 `y` to a byte
in X (y only moves between two u8 endpoints), which makes the y step an `ADC` on X and the
end test one compare. LLVM-MOS (767813) is behind Tisple here; cc65 (2.45M) far behind.

### sprites (1.08, Oscar64 with `__striped`)

With the array split per field, Tisple and Oscar64 make nearly the same code; Tisple's
automatic split is what Oscar64 needs `__striped` for (and without it Oscar64 takes
110384). The difference is reloads: Tisple stores the new x, then loads it twice more for
the two compares (`STA $4000,Y / LDA $4000,Y / CMP #8 / ... / LDA $4000,Y / CMP #240`);
Oscar64 compares the value still in A. gaps: 16336 cycles of "load already held"; without
them Tisple would take about 66K and win.

## Backend improvements, ranked

By cycles won across the kernels, then by how many kernels gain. Estimates are from the
listings and gaps runs above, on this commit.

1. **Strength reduction for u16 and while-loop induction variables.** The backend already
   steps pointers made from a u8 `for` counter (life's rows); extend it to u16 counters,
   to `while` loops whose variable moves by `(set v (+ v c))`, and to `(at arr v)` with u16
   v. Keep the pointer's low byte in Y as the index where it fits and bump the high byte on
   a wrap; when the base is page-aligned skip the low add (`ADC #$00`). Gains: strlen
   108K to about 40K, sieve 1.52M to about 1.0M, search about 100K, line about 145K (both
   COL tables, with the low bytes set once outside the loop), mandel's output pointer.
   Wins strlen, sieve and line against today's leaders.
2. **Multiply.** Narrow operands: zero-extended bytes take an 8x8 to 16 routine, and a
   value range like mandel's (|z| < 128 after the guard) a signed 8x8. Rewrite `rt.mul16`
   the way Oscar64's works: multiplier a byte at a time with a sentinel bit, stop when it
   is zero, skip a zero high byte, the high multiplier byte only reaching the result's
   high byte (an 8x8 to 8), the product's low byte in a register. Then quarter-square
   tables (floor(x*x/4), made by a macro like other tables; 2 KB for 8x8 to 16, about 40
   cycles) when code size allows. Gains: mul8 96K to about 63K (about 25K with tables),
   mul16 86K to about 65K, mandel 9.1M to about Oscar64's 7.6M, more with narrowing.
3. **Registers for loop variables and invariants.** Without calls in the loop, keep the
   inner counter in X or Y and a loop-invariant byte in A or the other index register:
   sort's j and v (40 to about 24 cycles a pass: 717K to about 440K), fill's v (14 to 11
   cycles a byte), search's j, crc16's bit counter as a count-down X.
4. **Reuse what A already holds after a store or a compare.** sprites reloads a field it
   just stored, twice (16K of 82K; would win), and line, mandel and crc16 have the same
   "load already held" / "store already held" findings in gaps.
5. **Fold constant offsets into absolute addresses.** `(at arr (- j 1))` is `arr-1,Y`, and
   `(at buf (+ C (as u16 i)))` with constant C and u8 i is `buf+C,X`. sort loses its
   `DEY/INY` pairs; `fill/unroll.tsp` and `memcpy/unroll.tsp` become the page-split loops
   (about 42K and 38K: ahead of cc65's 64K and 60K, and 2.6x faster than they are now).
6. **16-bit logic and compares by byte.** `(!= (& x 0x8000) 0)` is `BIT x+1 / BPL`; an AND
   or compare with a constant whose low byte is 0 or $FF needs only one byte; a signed
   compare with a constant checks the high byte first; `-128 < x < 128` is one test of the
   high byte against the low byte's sign. Shift a variable in place (`ASL x / ROL x+1`)
   when the result goes back to it, and hoist an expression both `if` arms compute
   (crc16's `(<< crc 1)`). Gains: crc16 540K to about 250K, mandel's guard, line's error
   tests.
7. **Unrolling.** Unroll small constant-trip loops (search's 6-byte compare; LLVM-MOS
   does, 2.3x faster), and page loops 2x or 4x the way cc65's library does. Optionally
   self-modifying `STA abs,Y` (5 cycles instead of 6; Oscar64's `-OM`), since Tisple
   programs run from RAM.
8. **Loop control.** Rotate loops instead of a `JMP` to the test; count down to zero when
   the counter's value is unused (`DEX / BNE`); `INC i / BPL` for a loop to 128; drop
   `CMP #$00` after a load; carry the exit condition out of a loop (search's second
   `j = 6` test); drop `CLC` before adds that cannot carry (life's 0/1 sums: 23K). A few
   percent on every kernel.
9. **Range facts that narrow variables.** lib/hgr's line keeps y in an i16 although it only
   moves between two u8 endpoints; Oscar64 holds it in X. Bigger work; line and similar
   coordinate code.
10. **Copies.** Bind inlined arguments to the caller's variables instead of copying them
   into the callee's frame (hgr.plot's x and y, every plot), and compute a `let` pointer
   straight into its variable (search's `ptrs` then `p`).

On the tools: `tisple gaps` finds the redundancies (items 4, 8 and the strided values of
item 1) but not structure, which is where most of the gap is: on sort, search, fill and
memcpy it finds nothing. `tisple gaps --patch FILE@ADDR` (new, `--sim` only) runs a
program on given memory, so `races/run.sh --gaps` profiles each kernel on the race's own
inputs; before, gaps ran them on zeroed buffers, where strlen's string was empty.

## Where the languages force different code

- No 32-bit type in Tisple, so mandel uses 6 fraction bits and tests |z| < 2 before
  squaring, so no product leaves 16 bits; the C versions do the same (C would otherwise
  use a 32-bit product and a shift).
- A u8 loop over all 256 values is `(for i u8 1 256 ...)` in Tisple and
  `do { ... } while (++i != 0)` in C, since `i < 256` never ends for a `uint8_t`.
- Tisple's `for` steps by one, so sieve's inner loop is a `while` in Tisple and a `for`
  with `k += i` in C; the same loop either way.
- C promotes to 16-bit `int`: life's 8-bit neighbour sum and every `uint8_t` add are int
  adds the compilers must narrow back. Tisple says the width.
- line: the Tisple kernel calls lib/hgr (tables made by macros, the screen reached through
  integer-made pointers, as the provenance rule wants). The C kernel is lib/hgr written in
  C with the same tables, the 16-bit row table split into low and high byte tables by hand,
  as a 6502 C programmer would.
- sprites: Tisple splits the array of structs per field by itself. Oscar64 needs
  `__striped`, and a striped array cannot have element pointers, so its fast build is
  `index.c` (`sprites[i].x`) rather than `kernel.c` (`struct sprite *s`).
- Oscar64 misreads `(uint16_t)a[i]` when `a` is a macro for a constant pointer (it applies
  `[i]` to the cast), so the C kernels write `(uint16_t)(a[i])`.

## Program race

Started 2026-10-30. The kernels are single loops, so this race takes ten programs of 24 to
447 lines, each written once in C and once in Tisple the way a programmer of that language
would write it: globals and `uint8_t` indexes in the C as cc65 users write it, the C
library where a C programmer would reach for it (`strcpy`, `strcmp`, `memcmp`, `memset`),
`match`, macros for tables and split struct arrays in the Tisple, and nothing in either
that the other cannot say. `races/run.sh --programs` runs them with the kernel race's
machinery (same simulator, flags, buffer modes, byte counts, fastest correct build per
compiler); `races/README.md`, "The program race", has the layout. Every program folds its
results into a u16 checksum at $5F00 the same way in both languages, and the harness checks
it and the output buffers against a Python reference (`spec.py` beside each program). A
wrong result is a failure, not a slow time.

| Program | What it does | Lines C / Tisple |
| --- | --- | ---: |
| bytesieve | BYTE's sieve as published (Gilbreath 1981): 10 passes, 8191 flags, `int` | 30 / 24 |
| sort | quicksort of 1024 words with an explicit stack and an insertion pass, a stable insertion sort of 64 records, a high-score table | 151 / 141 |
| dhrystone | Dhrystone 2.1, 16-bit ints, 200 runs | 239 / 248 |
| parser | text adventure: 300 typed commands upper-cased, split, looked up in a 46-word vocabulary, dispatched to verb handlers | 351 / 447 |
| lz4 | LZ4 block decompression of 8 KB (text and a tile map) | 55 / 62 |
| vm | a 16-bit stack machine's interpreter running 29443 bytecodes (sort, gcds, Fibonacci, hash, sieve) | 114 / 173 |
| bfs | breadth-first search on a 64 x 48 grid, 6 paths walked back and marked | 69 / 72 |
| blit | 24 masked 16 x 16 sprites shifted into a bitmap with save-under, 8 frames | 115 / 115 |
| scroll | a 128 x 24 tile map drawn at a pixel scroll, animated tiles, a player, 8 frames | 98 / 95 |
| bignum | 8-digit decimal scores and a high-score table, then 100! printed by bignum division | 138 / 128 |

Other ways each is written, raced too (the table names the one that won): `unsigned.c`
(bytesieve with unsigned variables), `loop.c` (parser's word compare as a loop, not
`memcmp`), `index.c` (lz4 with indexes, not pointers), `twoindex.c` (bignum's digits
reversed with two indexes), and `pointer.tsp` (lz4, blit and scroll with pointers instead of
u16 indexes, as their C has).

<!-- program race results: begin (races/run.sh --programs --update-doc writes this part) -->

Cycles / bytes, best setting of each compiler:

| Program | cc65 | llvm-mos | oscar64 | tisple | Winner | Tisple / best other |
| --- | ---: | ---: | ---: | ---: | --- | ---: |
| bytesieve | 20598829 / 328 | 22068852 / 4103 | 14349970 / 177 | 12090810 / 202 | tisple | 0.84 |
| sort | 3820411 / 2520 | 2192144 / 6261 | 1202552 / 1452 | 1317925 / 1715 | oscar64 | 1.10 |
| dhrystone | 2205531 / 3200 | 951470 / 2493 | 391865 / 1289 | 782966 / 2654 | oscar64 | 2.00 |
| parser | 8244051 / 2402 | 1782671 / 6951 | 2280481 / 1974 | 1433490 / 1752 | tisple | 0.80 |
| lz4 | 3199397 / 838 | 1212242 / 605 | 901828 / 372 | 1032306 / 449 | oscar64 | 1.14 |
| vm | 12676676 / 2775 | 7360089 / 4569 | 5628776 / 1137 | 4463209 / 1241 | tisple | 0.79 |
| bfs | 10098451 / 1074 | 5643078 / 2087 | 3157011 / 726 | 3413444 / 755 | oscar64 | 1.08 |
| blit | 6688330 / 1642 | 4499903 / 2278 | 2186965 / 873 | 2306473 / 727 | oscar64 | 1.05 |
| scroll | 45569306 / 1078 | 12854461 / 1813 | 8869763 / 459 | 9448752 / 684 | oscar64 | 1.07 |
| bignum | 10148189 / 1807 | 6656057 / 5322 | 3365122 / 840 | 1691976 / 1244 | tisple | 0.50 |
| geomean, x the fastest | 3.74x | 1.85x | 1.17x | 1.12x | | 0.981 |

### bytesieve: BYTE's sieve as published: 10 passes, 8191 flags, 1899 primes

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 20598829 | 328 | -Oirs -Cl const unsigned.c |
| llvm-mos | 22068852 | 4103 | -O3 extern |
| oscar64 | 14349970 | 177 | -O2 const unsigned.c |
| tisple | 12090810 | 202 | - |

Winner: tisple. Tisple / best other: 0.84.

### sort: Quicksort (explicit stack) and insertion sort of 1024 words, a stable record sort, a high-score table

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 3820411 | 2520 | -Oirs -Cl const |
| llvm-mos | 2192144 | 6261 | -O3 const |
| oscar64 | 1202552 | 1452 | -O3 const |
| tisple | 1317925 | 1715 | - |

Winner: oscar64. Tisple / best other: 1.10.

### dhrystone: Dhrystone 2.1, 16-bit ints, 200 runs

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 2205531 | 3200 | -Oirs -Cl const |
| llvm-mos | 951470 | 2493 | -O2 const |
| oscar64 | 391865 | 1289 | -O3 -Ox -OM const |
| tisple | 782966 | 2654 | - |

Winner: oscar64. Tisple / best other: 2.00.

### parser: Text adventure: 300 typed commands tokenized, looked up, dispatched

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 8244051 | 2402 | -Oirs -Cl const |
| llvm-mos | 1782671 | 6951 | -O3 const loop.c |
| oscar64 | 2280481 | 1974 | -O3 const loop.c |
| tisple | 1433490 | 1752 | - |

Winner: tisple. Tisple / best other: 0.80.

### lz4: LZ4 block decompression: 8 KB of text and tile map

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 3199397 | 838 | -Oirs -Cl const |
| llvm-mos | 1212242 | 605 | -O3 extern |
| oscar64 | 901828 | 372 | -O2 const |
| tisple | 1032306 | 449 | - |

Winner: oscar64. Tisple / best other: 1.14.

### vm: Bytecode interpreter: a stack machine sorting, gcds, Fibonacci, sieve

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 12676676 | 2775 | -Oirs -Cl extern |
| llvm-mos | 7360089 | 4569 | -O2 const |
| oscar64 | 5628776 | 1137 | -O3 -Ox -OM const |
| tisple | 4463209 | 1241 | - |

Winner: tisple. Tisple / best other: 0.79.

### bfs: Pathfinding: breadth-first search on a 64 x 48 grid, 6 paths

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 10098451 | 1074 | -Oirs -Cl const |
| llvm-mos | 5643078 | 2087 | -O2 const |
| oscar64 | 3157011 | 726 | -O3 const |
| tisple | 3413444 | 755 | - |

Winner: oscar64. Tisple / best other: 1.08.

### blit: Sprite blitter: 24 masked 16 x 16 sprites shifted into a bitmap, background saved and restored, 8 frames

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 6688330 | 1642 | -Oirs -Cl const |
| llvm-mos | 4499903 | 2278 | -O3 extern |
| oscar64 | 2186965 | 873 | -O3 -Ox -OM const |
| tisple | 2306473 | 727 | pointer.tsp |

Winner: oscar64. Tisple / best other: 1.05.

### scroll: Tile map scroller: 32 x 24 tiles of a 128-wide map at pixel scroll, animated tiles, a player, 8 frames

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 45569306 | 1078 | -Oirs -Cl const |
| llvm-mos | 12854461 | 1813 | -O2 extern |
| oscar64 | 8869763 | 459 | -O3 const |
| tisple | 9448752 | 684 | pointer.tsp |

Winner: oscar64. Tisple / best other: 1.07.

### bignum: Decimal scores and a high-score table, then 100! printed by bignum division

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 10148189 | 1807 | -Oirs -Cl extern twoindex.c |
| llvm-mos | 6656057 | 5322 | -O2 const twoindex.c |
| oscar64 | 3365122 | 840 | -O2 striped twoindex.c |
| tisple | 1691976 | 1244 | - |

Winner: tisple. Tisple / best other: 0.50.

<!-- program race results: end -->

### After the fixes (2026-10-30, branch program-race-fixes)

Tisple runs all ten now: fastest on four (bignum 0.50, vm 0.79, parser 0.80, bytesieve
0.84), behind Oscar64 on six (blit 1.05, scroll 1.07, bfs 1.08, sort 1.10, lz4 1.14,
dhrystone 2.00); geometric mean 0.981 of the fastest other (was 1.070 on nine). What
changed, each a general rule (the commits have the numbers per program):

- **sort**: the miscompile (bug 1 below) is fixed: 1.10.
- **Warm inlining** (bfs 1.53 to 1.14, scroll 1.43 to 1.08): a function over the 24-node
  limit, up to 48, with no loops or calls, called in more than one place and run at least as
  often as its extra copies add nodes, is copied at every call by the backend; the size
  budget keeps the copies only when they save its hot cycles a byte, and counts them against
  the function and program budgets. bfs's `visit` (29 nodes) and scroll's `tile_line` (25)
  are copied. The rule that skips a small function given a for's counter (`inline.cpp`) was
  tried without: nothing in the program or kernel races changes, and 8 of 2000 random
  programs get slower against 3 faster, so it stays.
- **lz4** (1.31 to 1.14): a step of a byte nothing reads afterwards but more such steps goes
  (`m`, read only through its step pointer, dead after the match copy); a u16 count down
  tests its high byte only when the low byte reaches 0 (`DEC lo / BNE top / LDA hi / BNE
  top`); and `(set x (+ x (as u16 b)))` moves the high byte by `BCC / INC`, which is also
  every program's checksum loop (a third of lz4's time).
- **dhrystone** (2.19 to 2.00): a global pointer nothing sets is the address it starts
  with (`ptr_glob` is `(addr_of glob_rec)`), as is a pointer local one let sets to a
  variable's address (an inlined call's pointer parameter); loads and stores through
  `(addr_of x)` read and write x directly; the 37-byte record copy is a `LDA abs,Y / STA
  (p),Y / DEY / BPL` loop, 15 cycles a byte (was 21 through zero page).
- **blit** (1.08 to 1.05): the backend's value ranges have smallest values too, so `back`
  (1 to 8) needs no test for a shift by 0. The test for 8 or more stays: the loop would give
  the same 0, but scroll, whose count is 8 one time in eight, is faster with it.
- **bfs, sort** (page pointers): a second `(at table i)` of the same table and index reuses
  the pointer the first set up while its high byte holds, a u16 element of a split array
  takes one page pointer a plane, and a function keeps 6 of them.

Left: Dhrystone's main gap is Oscar64 inlining everything into main and folding through it
(`Int_1_Loc` is 3 whenever `Proc_8` runs: Tisple's constant propagation does not follow a
local whose address is taken, and does not run a loop whose inputs are all known); its
string copies of constants are byte loops where the C compilers copy a fixed length. lz4's
`o` is still stepped beside its pointer (it is read after the loop) and its u16 count is
in memory, not X. Kernels: as on main, but mandel is 4180008 (4177022): its dead `k` steps
go and a branch in the loop now crosses a page.

### Where Tisple lost, and why (2026-10-30, 02e5697, before the fixes)

Tisple is fastest on four programs (bignum 0.51, vm 0.79, parser 0.81, bytesieve 0.84),
behind Oscar64 on five (blit 1.08, lz4 1.31, scroll 1.43, bfs 1.53, dhrystone 2.19), and
miscompiles sort. The geometric mean of Tisple over the fastest other compiler is 1.07 on
the nine it runs: the kernels' 0.6 to 0.9 does not carry over to whole programs. Oscar64 is
again the compiler to beat; cc65 is 1.7x to 5.6x behind the fastest (3.7x in the
geometric mean). From `tisple race --profile`
and the listings:

- **dhrystone 2.19.** Oscar64 inlines every procedure into main and then sees that
  `Ptr_Glob` is only ever `&Glob_Rec` and that `Int_1_Loc` is 3 whenever Proc_8 is called:
  the record fields and `Arr_2_Glob[8][7]` become absolute addresses (`INC $6398`), the
  `* 50` row multiplies disappear. Tisple keeps `ptr_glob` a pointer read through `(p),Y`,
  and `int_1_loc` has its address taken (`proc_2`), so nothing about it is known: proc_8's
  `(* (as u16 int_loc) 50)` is a multiply every run. The record copy `*Ptr_Comp = *Ptr_Glob`
  (37 bytes) costs Tisple 797 cycles a run, 19% of its time: it loads every byte into zero
  page and then stores every byte through the pointer, 21 cycles a byte; Oscar64 copies
  with `LDA abs,Y / STA abs,Y` at 14. Tisple's `str_copy` loop (16 cycles a byte) is as good
  as a library `strcpy`; the C compilers turn the copies of constant strings into
  fixed-length copies (Oscar64 inline, LLVM-MOS a `memcpy` call).
- **bfs 1.53.** `visit` (4 calls a cell) is 57% of the time and is not inlined: it is over
  the 24-node limit for every-call inlining (`kMaxInlineExprs`). Each call pays the JSR, the
  RTS, its arguments, and sets up its own pointers from scratch (the zero low bytes of three
  plane pointers, `came_from`'s high byte twice, two pointers into the split `queue`).
  Oscar64 inlines all four and reaches the neighbours from one cell pointer with `Y`
  offsets.
- **scroll 1.43** (`pointer.tsp`; 1.54 with indexes). `tile_line`, called twice a screen
  byte, is not inlined: the inliner skips a small function given a for's counter on
  purpose (`inline.cpp`, for life's rows). Its body is a third of the time, and each call
  copies three arguments and pays a JSR and an RTS; the shifts are the same loops Oscar64
  emits (`ASL / DEX / BNE`). Oscar64 inlines it.
- **lz4 1.31.** The match copy, `(set (at out o) (at out m))` with u16 `o`, `m` and `len`,
  is 63 cycles a byte against Oscar64's 44: the loop's pointers replace the indexes for the
  load and the store, but `o` and `m` are still stepped every byte as well (`m` is dead
  after the loop), and the u16 count is decremented and tested with `ORA` each pass, where
  Oscar64 keeps its low byte in X. The pointer version (`pointer.tsp`, 1193968) is no
  faster: `(set src (+ src 1))` on a pointer global is a full 16-bit add, not an `INC`.
- **blit 1.08** (`pointer.tsp`; 1.31 with u16 indexes into the screen). The variable shifts
  are the same loops as Oscar64's, but every `(<< x back)` also tests for a count of 8 or
  more (Tisple defines a shift by the width as 0, and `back` is 1 to 8, which the backend
  does not know): 6 more cycles a shift, four such shifts a sprite row.

The common thread is the inliner. Oscar64 inlines small procedures into the hot loop and
then folds what the call site knows; Tisple's limit (24 nodes, no small function in a for's
counter) keeps `visit` and `tile_line` as calls. Second, induction variables that the
pointer strength reduction leaves stepping when the pointer has replaced them (lz4).
Third, aggregate copies through zero page.

Where Tisple wins: bignum's `(/ v 10)` and `(% v 10)` by a constant are done inline (41%
of its time) while the C compilers call their 16-bit divide (Oscar64's `divmod` is 60% of
its); vm's `match` on 26 opcodes is a jump table with the stack in split byte arrays; the
parser's tables are split per field so the vocabulary scan is `LDA table,X`.

### Compiler bugs found (2026-10-30)

Reduced in the race; Tisple's fixed since.

1. **Tisple: a store after an inner loop goes to a stale address** (sort; fixed). An element
   pointer the outer loop made for `(at keys j)` is not refreshed after an inner `while`
   moves `j`, so the store lands where `j` was; the interpreter gives 353, the build 1:

   ```scheme
   (let keys (array u16 300))
   (fn f ((hi u16)) u16
       (let i u16 0)
       (let j u16 hi)
       (while true
           (while (< (at keys i) 500)
               (set i (+ i 1)))
           (while (> (at keys j) 500)
               (set j (- j 1)))
           (if (>= i j)
               (return j))
           (set (at keys j) 0xFFFF)
           (set j (- j 1)))
       j)
   (fn main () u16
       (for i u16 0 300 (set (at keys i) (& (* i 97) 0x3FF)))
       (let r u16 (f 299))
       (let sum u16 r)
       (for i u16 0 300 (set sum (+ (+ sum sum) (at keys i))))
       sum)
   ```

   `tisple interp` returns 353 (Python agrees), `tisple run --sim` 1. Without the first
   inner `while`, or without the store, the two agree. The program's quicksort never ended.

   Fixed on branch program-race-fixes (f8df59e), at -O0 too. The outer loop steps `j` only
   by constants, so `(at keys j)` goes through a step pointer the outer loop owns
   (`gen_step.cpp`), moved wherever `j` is stepped. The inner `while` kept `j` in Y (a wide
   loop): its steps were `DEY`, and it stored `j` back where it ended, so the outer loop's
   pointer stayed where `j` was before the inner loop. The inner loop now notes where it
   stores the variable back, and the outer loop's pointers of it are set again there. The
   reduction is a test (`test_backend_loop_ptrs.cpp`); the fuzzer's `(partition)` programs,
   also at -O0, have the shape. The `ADC` of the high plane's offset noted here before is
   right: only the `BCC` taken reaches it, with the carry clear.
2. **Oscar64: an in-place reverse keeps a stale high byte** (bignum, every setting).
   `for (i = 0; i < n / 2; i++) { t = dec[i]; dec[i] = dec[n - 1 - i]; dec[n - 1 - i] = t; }`
   with `dec` a constant pointer: the 16-bit temporary for `n - 1 - i` keeps the pointer's
   high byte from the last pass, so every odd element gets 0. `twoindex.c` (`i` and `j`
   meeting) is right, and is where Oscar64's bignum time comes from.
3. **Oscar64: a `__striped` store at index 256** (sort, every setting with `striped`).
   Insertion sort's `keys[j] = keys[j - 1]` on a striped u16 array addresses `keys[j]` as
   `(keys - 1 + 256 * hi(j)),Y` with `Y = lo(j) + 1` by `INY`, which wraps when `lo(j)` is
   255.
4. **Oscar64 -O2: sort's quicksort never ends** (-O3 is right). Needs the median of three;
   reduced to a 60-line program (partition, the explicit stack and the loop), not further.
5. **Oscar64 -O2: Dhrystone's Int_2_Loc ends 9, not 13** (-O3 is right): the run's last
   `Int_2_Loc = 7 * (Int_2_Loc - Int_3_Loc) - Int_1_Loc` is lost. Not reduced below the
   program.
6. **Oscar64: a local named like a struct tag hides the tag**: `struct score { int score; };`
   then `int score; struct score *p;` in a function is error 3013 "Struct expected". The
   sort program's struct is `entry` for that reason.
7. **Oscar64 has no `register` keyword** (error 3005); `race.h` defines it away for Oscar64.

## Random programs

Started 2026-10-27. Thirteen kernels are a narrow claim, so the random race takes the fuzz
generator's programs (`tests/fuzz_gen*.cpp`, the ones the backend is fuzzed with), writes
each one in C as well, and races all four compilers on every one, each at its best kernel
setting: cc65 `-Oirs -Cl`, LLVM-MOS `-O2`, Oscar64 `-O2`. `races/README.md`, "Random
programs", has how a program is made, timed and checked, and the commands, long runs on the
Mac mini included; `races/random.sh --report --update-doc` rewrites the part between the
markers below.

In short: `tisple race-gen` writes the seed's program in race mode (no I/O; the generated
main is `t.run`), the same program in C (`src/race/race_c.hpp`: exact widths, Tisple's
wrapping and its results for shifts past the width and division by zero spelled out, left
to right evaluation kept with temporaries), and the interpreter's checksum of every global.
Start values are set by `t.init` from an opaque 0 no compiler can see through, as the
kernels' inputs are written at main's entry. The clock runs from `t.init`'s return to
`t.checksum`'s entry. Every compiler's checksum must be the interpreter's; when a C compiler
is wrong, the C is first checked on the host under the address and undefined-behaviour
sanitizers, so a fault in our C is never blamed on a compiler.

<!-- random race: begin (races/random.sh --report --update-doc writes this part) -->

2000 programs (seeds 1..2000): 2000 ok; 2000 raced (Tisple and a C compiler right).

Tisple against the fastest other compiler, per program: wins 122 (6.1%), ties 2 (0.1%), losses 1876 (93.8%). Geometric mean of the ratios 3.087.

| Percentile | 1 | 5 | 10 | 25 | 50 | 75 | 90 | 95 | 99 | max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Tisple / best other | 0.72 | 0.95 | 1.14 | 1.56 | 2.51 | 4.81 | 11.25 | 21.41 | 79.99 | 1427.52 |

| Ratio | < 0.50 | 0.50-0.75 | 0.75-0.90 | 0.90-1.00 | 1.00 | 1.00-1.10 | 1.10-1.25 | 1.25-1.50 | 1.50-2.00 | >= 2.00 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Programs | 4 | 25 | 52 | 41 | 2 | 52 | 105 | 179 | 306 | 1234 |

Against each compiler:

| Against | Programs | Tisple faster | Cycles, geomean of Tisple / it | Bytes, geomean of Tisple / it | Fastest of all |
| --- | ---: | ---: | ---: | ---: | ---: |
| cc65 | 1965 | 97.3% | 0.399 | 0.551 | 0.0% |
| llvm-mos | 1998 | 12.9% | 2.766 | 1.481 | 63.5% |
| oscar64 | 1930 | 11.9% | 1.961 | 1.492 | 30.3% |

By where the work is (the feature family with the largest share of t.run's weighed work), worst first:

| Most of the work is | Programs | Tisple loses | Geomean ratio | Median ratio | Fastest most often |
| --- | ---: | ---: | ---: | ---: | --- |
| divide | 889 | 97.0% | 4.00 | 3.38 | llvm-mos (72.0%) |
| u8 index | 59 | 98.3% | 3.67 | 2.67 | llvm-mos (64.4%) |
| match | 13 | 100.0% | 3.63 | 1.82 | llvm-mos (84.6%) |
| call | 28 | 96.4% | 3.55 | 2.78 | llvm-mos (53.6%) |
| pointer | 56 | 98.2% | 3.27 | 2.26 | llvm-mos (75.0%) |
| 16-bit for | 13 | 100.0% | 3.22 | 3.00 | llvm-mos (69.2%) |
| multiply | 238 | 91.2% | 2.77 | 2.55 | llvm-mos (63.0%) |
| 8-bit &|^ | 107 | 99.1% | 2.62 | 1.99 | llvm-mos (55.1%) |
| 8-bit +- | 58 | 89.7% | 2.37 | 2.02 | llvm-mos (65.5%) |
| widen | 100 | 98.0% | 2.37 | 2.11 | llvm-mos (55.0%) |
| 16-bit &|^ | 30 | 83.3% | 2.37 | 1.79 | llvm-mos (43.3%) |
| shift by var | 70 | 94.3% | 2.36 | 1.81 | oscar64 (50.0%) |
| unsigned compare | 102 | 76.5% | 2.35 | 1.74 | llvm-mos (54.9%) |
| shift by const | 7 | 100.0% | 2.29 | 2.01 | llvm-mos (71.4%) |
| narrow | 7 | 100.0% | 2.26 | 2.24 | llvm-mos (57.1%) |
| u16 index | 26 | 69.2% | 2.13 | 1.45 | llvm-mos (42.3%) |


| The worst 100 losses, by their two heaviest families | Programs | Geomean ratio |
| --- | ---: | ---: |
| divide + multiply | 22 | 53.76 |
| 8-bit &|^ + divide | 6 | 35.32 |
| call + divide | 6 | 56.60 |
| 16-bit &|^ + divide | 5 | 49.48 |
| divide + unsigned compare | 5 | 36.60 |
| divide + widen | 5 | 29.44 |
| 16-bit +- + divide | 4 | 54.86 |
| divide + shift by var | 4 | 40.95 |
| divide + u8 index | 4 | 40.87 |
| 16-bit +- + unsigned compare | 2 | 67.40 |

Worst losses:

| Seed | Ratio | Fastest | Tisple cycles | Its cycles | Where the work is |
| ---: | ---: | --- | ---: | ---: | --- |
| 1740 | 1427.52 | llvm-mos | 204135 | 143 | unsigned compare 36%, 8-bit +- 27%, while 9% |
| 1241 | 311.64 | llvm-mos | 29606 | 95 | divide 84%, 16-bit &|^ 2%, 8-bit &|^ 2% |
| 567 | 220.67 | llvm-mos | 20302 | 92 | divide 89%, 16-bit +- 2%, 16-bit &|^ 2% |
| 1114 | 194.69 | llvm-mos | 60548 | 311 | divide 80%, call 4%, widen 2% |
| 50 | 184.03 | llvm-mos | 50423 | 274 | divide 37%, multiply 37%, 8-bit &|^ 7% |
| 745 | 135.42 | llvm-mos | 40490 | 299 | divide 71%, multiply 9%, unsigned compare 3% |
| 1326 | 126.87 | llvm-mos | 5963 | 47 | divide 67%, multiply 8%, shift by var 4% |
| 741 | 126.23 | llvm-mos | 35093 | 278 | 16-bit +- 36%, unsigned compare 27%, u8 index 18% |
| 1922 | 126.16 | llvm-mos | 8074 | 64 | multiply 72%, 8-bit +- 7%, divide 4% |
| 1559 | 122.09 | llvm-mos | 14162 | 116 | multiply 38%, u8 index 13%, 8-bit &|^ 11% |
| 76 | 119.29 | llvm-mos | 23500 | 197 | u16 index 21%, u8 index 16%, if 12% |
| 1467 | 117.00 | llvm-mos | 2574 | 22 | divide 66%, multiply 22%, shift by var 3% |
| 1728 | 110.25 | llvm-mos | 1323 | 12 | 8-bit &|^ 28%, u8 index 23%, unsigned compare 12% |
| 161 | 108.87 | llvm-mos | 16004 | 147 | multiply 73%, divide 6%, pointer 4% |
| 1349 | 99.77 | llvm-mos | 20354 | 204 | divide 57%, shift by var 14%, 8-bit &|^ 5% |

Bugs (a compiler whose checksum is not the interpreter's, or that failed to build or run the program):

| Who is wrong | What | Programs | Seeds |
| --- | --- | ---: | --- |
| cc65 | build or run failed | 4 | 118, 243, 866, 1002 |
| cc65 | wrong checksum | 31 | 32, 96, 128, 165, 233, 325, 518, 553, 557, 758, 917, 933 ... |
| llvm-mos | build or run failed | 2 | 1587, 1855 |
| oscar64 | build or run failed | 26 | 118, 164, 374, 500, 518, 620, 631, 688, 794, 841, 892, 939 ... |
| oscar64 | wrong checksum | 44 | 85, 96, 127, 181, 324, 341, 362, 404, 459, 464, 578, 648 ... |

<!-- random race: end -->

### What loses, and why (2026-10-27, seeds 1 to 2000)

Tisple, fastest on all 13 kernels (by 6% to 2.2x), is about 3x behind the fastest C
compiler on random programs (the geometric mean; the median program 2.5x), and the reason
is mostly not code generation: the generator's programs are full of work whose result
nobody reads, and LLVM-MOS and Oscar64 delete it while Tisple compiles every form it is
given. The losses by shape are losses by how much dead or repeated work a program carries,
and a division is the most expensive work to carry, so the programs whose work is mostly
dividing lose most (4x). Typical cases, each in `races/out/random/worst/` after a
`--report`:

- **Dead loops.** Seed 1740 (1427x): `(for w35 u16 61 486 (let j36 u8 0) (while (< j36 3)
  (set j36 (+ j36 1))) (if (< n32 127) (continue)))` does nothing a later form reads, 12
  times over; LLVM-MOS runs the program in 143 cycles, Tisple in 204135. Seed 25 (2.5x
  against Oscar64) spends most of its time in a 289-pass loop around an empty 3-pass while
  and a match with empty arms.
- **Dead values.** Seed 285 (37x against Oscar64): `(let p24 (ptr u8) (addr_of (at pt (&
  (/ g1 ...) 7))))` is never read, so its division goes; and `(set g5 (% g5 9)) (set g5
  (/ g5 9))` five times is `g5 = 0` once a compiler knows `x % 9 < 9`. Oscar64 takes 45
  cycles, LLVM-MOS 54, Tisple 1661.
- **Loop-invariant work.** Seed 23 (45x): a 7-pass for whose body, and its inner 3-pass
  while, store the same divisions every pass; LLVM-MOS takes 521 cycles, Tisple 23680.

Most of the worst hundred losses (`--report`'s table of their two heaviest families) have
divide among them. So the next backend items the random race asks for are mid-level,
before code generation: removing stores and lets nobody reads, deleting loops with no
effects, hoisting loop-invariant pure expressions (divisions first), and value ranges
(`x % 9 < 9`). Real programs carry less dead work than these, so the kernels stay the
measure of code generation; the random race measures what happens to code nobody tuned,
and finds the shapes. Tisple is still faster than cc65 on 97% of the programs, and faster
than all three on 6%.

### C compiler bugs found

Each a program where that compiler's checksum is not the interpreter's (and the host build
of the C agrees with the interpreter), or that it fails to build or run; the programs are
kept in `races/out/random/bugs/<seed>/` with every build. Two reduced by hand:

- **cc65 V2.19** drops the cast in `(uint8_t)~x` when the value is used wider, at every
  optimisation level: `uint8_t g = 0x12; uint16_t s = 0x100; s = s + (uint8_t)~g;` gives
  237 ($00ED, it added $FFED), not 493 ($01ED). 29 of the 31 programs cc65 gets wrong
  use `~` on a byte. Seed 32 is wrong only with `-O` (right with `-Cl` alone), and the
  compiler dies with signal 11 on 4 programs (seeds 118, 243, 866, 1002).
- **Oscar64 v1.32.273** folds `(uint8_t)(c + 255)` as 256 when it knows `c` is 1, even at
  `-O0`: `c = 0; while (c < 1) { c = (uint8_t)(c + 1); if (g <= (uint8_t)(c + 255)) ...`
  takes the branch for g = 5 (seed 181). 44 programs in all have a wrong checksum, some
  only when optimising (seeds 85, 96, 127: right at `-O0`); internal assertions fail on 22
  (`NumberSet.h:64 elem >= 0 && elem < size` on 15, `mEntryRequiredTemps` on 4, and
  `ExpandingArray::Last size > 0`, a linker object's address, `mNumEntries`); 3 never
  finish compiling (seeds 1089, 1453, 1496: the race gives a build 60 s); seed 1989's
  binary runs past the 400M-cycle limit. It also rejects `(p + n)[k]` ("Array expected
  for indexing"), so the C writer writes `*(p + n + k)` instead.
- **LLVM-MOS** (llvm-mos-sdk v23.4.0) fails "ran out of registers during register
  allocation in function 'main'" on two programs (seeds 1587, 1855). Its checksums were
  all right.

No program needed the C writer blamed: all 2000 were also built for the host with the
sanitizers (`--host`), and every host checksum was the interpreter's.

### Caveats

- The race is only as varied as the generator: no recursion, no 32-bit values, small
  arrays, programs of tens of cycles to two million. Half the programs run in under
  1000 cycles on Tisple (the median is 786), where a call or two decides the ratio.
- The shape signature is crude: features weighed by how often the interpreter ran them and
  a guessed cost each, so "divide 80%" means the program spends its time dividing, not that
  Tisple's division is slow (it is not; the kernels race it).
- Bytes count whole functions that ran while the clock ran, as in the kernel race; cc65's
  module is cut at its function labels.
