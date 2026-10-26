# Compiler races

Started 2026-10-26. Tisple against the best 6502 C compilers, cc65 V2.19, LLVM-MOS
(llvm-mos-sdk v23.4.0) and Oscar64 v1.32.273, on 13 kernels. Every binary runs in our own
cycle-exact simulator, so all four are on one clock. `races/README.md` has the versions,
download URLs, flags and the timing rules; `races/run.sh` reruns everything and prints the
tables below. Add a dated note when something changes; regenerate the tables with
`races/run.sh --update-doc`.

In short: the kernel is `main` in every language. Its inputs are written into fixed
buffers when main is entered, so no compiler can fold them, and the cycles count from
main's first instruction to its RTS. Every output is checked against a Python reference.
Each C compiler is tried with several speed settings and two ways of declaring the
buffers, and the C side also gets its library (`memset`, `memcpy`, `strlen`) as an
alternative to the loop. The fastest correct build is the score. Bytes are all the code
the kernel ran, library routines included.

## Results

Cycles / bytes, 2026-10-26, commit after `a6dc31e` (this branch's backend is that
commit's). Ratio is Tisple's cycles over the fastest other compiler's; below 1 Tisple wins.

<!-- race results: begin (races/run.sh --update-doc writes this part) -->

Cycles / bytes, best setting of each compiler:

| Kernel | cc65 | llvm-mos | oscar64 | tisple | Winner | Tisple / best other |
| --- | ---: | ---: | ---: | ---: | --- | ---: |
| mul8 | 80903 / 184 | 101512 / 114 | 63165 / 113 | 95624 / 105 | oscar64 | 1.51 |
| mul16 | 101818 / 299 | 90156 / 130 | 65477 / 131 | 86293 / 111 | oscar64 | 1.32 |
| fill | 63996 / 165 | 238896 / 58 | 90467 / 57 | 115266 / 39 | cc65 | 1.80 |
| memcpy | 59878 / 143 | 153449 / 75 | 65824 / 69 | 65930 / 47 | cc65 | 1.10 |
| sieve | 2285731 / 210 | 2510574 / 250 | 1258060 / 128 | 1515066 / 151 | oscar64 | 1.20 |
| strlen | 36153 / 39 | 186081 / 75 | 39103 / 36 | 108102 / 45 | cc65 | 2.99 |
| search | 536420 / 145 | 136618 / 142 | 150500 / 63 | 317417 / 94 | llvm-mos | 2.32 |
| sort | 2020054 / 122 | 463713 / 56 | 422725 / 47 | 717510 / 50 | oscar64 | 1.70 |
| crc16 | 650111 / 218 | 257916 / 253 | 221527 / 78 | 540509 / 124 | oscar64 | 2.44 |
| mandel | 11216044 / 656 | 9969128 / 582 | 7601337 / 422 | 9149633 / 446 | oscar64 | 1.20 |
| life | 1333565 / 689 | 232244 / 163 | 238566 / 163 | 160650 / 403 | tisple | 0.69 |
| line | 2451456 / 805 | 767813 / 405 | 576133 / 335 | 705567 / 379 | oscar64 | 1.22 |
| sprites | 819546 / 424 | 117496 / 119 | 76125 / 85 | 82295 / 92 | oscar64 | 1.08 |

### mul8: 8-bit multiply: 256 byte products summed in 16 bits

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 80903 | 184 | -Oirs -Cl const |
| llvm-mos | 101512 | 114 | -O2 const |
| oscar64 | 63165 | 113 | -O2 const |
| tisple | 95624 | 105 | - |

Winner: oscar64. Tisple / best other: 1.51.

### mul16: 16-bit multiply: 128 word products summed, wrapping

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 101818 | 299 | -Oirs -Cl const |
| llvm-mos | 90156 | 130 | -O2 const |
| oscar64 | 65477 | 131 | -O2 striped |
| tisple | 86293 | 111 | - |

Winner: oscar64. Tisple / best other: 1.32.

### fill: Fill 8 KB with a byte

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 63996 | 165 | -Oirs -Cl const lib.c |
| llvm-mos | 238896 | 58 | -O2 const |
| oscar64 | 90467 | 57 | -O2 const lib.c |
| tisple | 115266 | 39 | - |

Winner: cc65. Tisple / best other: 1.80.

### memcpy: Copy 4 KB

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 59878 | 143 | -Oirs -Cl const lib.c |
| llvm-mos | 153449 | 75 | -O3 const |
| oscar64 | 65824 | 69 | -O2 const lib.c |
| tisple | 65930 | 47 | - |

Winner: cc65. Tisple / best other: 1.10.

### sieve: Sieve of Eratosthenes, 8192 flags, count the primes

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 2285731 | 210 | -Oirs -Cl const |
| llvm-mos | 2510574 | 250 | -O2 extern |
| oscar64 | 1258060 | 128 | -O2 const |
| tisple | 1515066 | 151 | - |

Winner: oscar64. Tisple / best other: 1.20.

### strlen: Length of a 3000-byte string

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 36153 | 39 | -Oirs -Cl const lib.c |
| llvm-mos | 186081 | 75 | -O2 const |
| oscar64 | 39103 | 36 | -O2 const |
| tisple | 108102 | 45 | - |

Winner: cc65. Tisple / best other: 2.99.

### search: Naive search for 6 bytes in 4 KB of text (found at 3500)

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 536420 | 145 | -Oirs -Cl const |
| llvm-mos | 136618 | 142 | -O3 extern |
| oscar64 | 150500 | 63 | -O2 const |
| tisple | 317417 | 94 | - |

Winner: llvm-mos. Tisple / best other: 2.32.

### sort: Insertion sort of 256 bytes

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 2020054 | 122 | -Oirs -Cl const |
| llvm-mos | 463713 | 56 | -O2 const |
| oscar64 | 422725 | 47 | -O2 const |
| tisple | 717510 | 50 | - |

Winner: oscar64. Tisple / best other: 1.70.

### crc16: CRC-16-CCITT, bit by bit, over 1 KB

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 650111 | 218 | -Oirs -Cl const |
| llvm-mos | 257916 | 253 | -O3 const |
| oscar64 | 221527 | 78 | -O2 const |
| tisple | 540509 | 124 | - |

Winner: oscar64. Tisple / best other: 2.44.

### mandel: Mandelbrot, 32 x 24 pixels, 16-bit fixed point (6 fraction bits)

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 11216044 | 656 | -Oirs -Cl const |
| llvm-mos | 9969128 | 582 | -O2 extern |
| oscar64 | 7601337 | 422 | -O2 const |
| tisple | 9149633 | 446 | - |

Winner: oscar64. Tisple / best other: 1.20.

### life: One Life step, 40 x 48 cells, as programs/life

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 1333565 | 689 | -Oirs -Cl const |
| llvm-mos | 232244 | 163 | -O2 extern |
| oscar64 | 238566 | 163 | -O2 const |
| tisple | 160650 | 403 | - |

Winner: tisple. Tisple / best other: 0.69.

### line: 24 hi-res lines (Bresenham and table plot, as lib/hgr)

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 2451456 | 805 | -Oirs -Cl const |
| llvm-mos | 767813 | 405 | -O2 const |
| oscar64 | 576133 | 335 | -O2 striped |
| tisple | 705567 | 379 | - |

Winner: oscar64. Tisple / best other: 1.22.

### sprites: Move and bounce 64 sprites (array of structs) for 16 frames

| Compiler | Cycles | Bytes | Best setting |
| --- | ---: | ---: | --- |
| cc65 | 819546 | 424 | -Oirs -Cl extern |
| llvm-mos | 117496 | 119 | -O2 extern |
| oscar64 | 76125 | 85 | -O2 striped index.c |
| tisple | 82295 | 92 | - |

Winner: oscar64. Tisple / best other: 1.08.

<!-- race results: end -->

## Where Tisple wins and loses

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

## Later: random-program races

The fuzz generator (`tests/fuzz_gen*.cpp`) writes random valid Tisple programs; it could
also write the same program in C, so the race gets thousands of programs nobody tuned.
Not built yet; the plan:

1. Emit C beside the Tisple text from the same generator state: one `uint8_t`/`int8_t`/
   `uint16_t`/`int16_t` per type, structs as structs, enums as `uint8_t`, `match` as
   `switch`, `for` as `for`, pointers as pointers (Tisple's `(+ p n)` already moves by
   elements, like C).
2. Write every operation so C means exactly Tisple's wrapping semantics: arithmetic on the
   unsigned type with a cast back (`(uint8_t)(a + b)`, signed through `uint16_t` then
   back), since signed overflow is undefined in C; shifts by the width or more and
   division by zero as small inline helpers that give Tisple's defined results; `>>` on
   signed values through a helper rather than relying on implementation-defined shifts.
3. I/O: `io_read`/`io_write` at `$C0xx` become `volatile` accesses; `tisple race` would get
   an I/O hook giving `IoReadValue`'s fixed pattern, as `sim_run` does.
4. Result: generated code at the end of main folds every global into a 16-bit checksum and
   stores it at a fixed address, in both languages, so no layout mapping is needed; the
   reference is the interpreter's checksum.
5. Timing as now: main entry to RTS. Report the sum and geometric mean of the cycle
   ratios per compiler over N seeds, and the seeds where Tisple loses most, each a ready
   test case for `tisple gaps --fuzz` and the backend.
