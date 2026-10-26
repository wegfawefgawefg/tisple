# Dogfood log

Issues found while writing real programs in Tisple. Fixed in batches; each
entry says what happened and what it should do.

## Open

1. `(store p 0)`: the literal gets no type, yet `(as u8 0)` is rejected as
   converting nothing. The stored value should take the pointer's element
   type as context, like `set` does. (lib/lores.tsp `clear`)
2. `(+ p x)` with `x : u8` must be written `(+ p (as u16 x))`. A `u8` index on
   a pointer is the most common 6502 idiom (`LDA (p),Y`); allow `u8` and `u16`
   counts directly.
3. The formatter puts a `const` table's type on its own line and packs the
   values to 100 columns, losing hand grouping (8 row addresses per line in
   lib/lores.tsp). Keep the type on the head line; maybe keep the author's
   line breaks inside data lists.

## Fixed

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

## Benchmarks

| Program | Measure | Result | Date |
| --- | --- | --- | --- |
| life | generations in 10 M cycles (about 10 s on a IIe) | 10 | 2026-10-26 |
| life | the same, byte indices as (p),Y (item 4) | 43 | 2026-10-26 |

