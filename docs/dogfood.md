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
4. `(load (+ p (as u16 e)))` with `e : u8` and `p` in zero page computes a full
   16-bit pointer sum per access (about 23 cycles) instead of `LDY e` /
   `LDA (p),Y` (5 cycles, Y reused). Life's inner loop does 9 such reads per
   cell: about 520 cycles per cell, 1 generation per second. Same semantics:
   the widened byte is exactly what Y holds. (programs/life)

## Benchmarks

| Program | Measure | Result | Date |
| --- | --- | --- | --- |
| life | generations in 10 M cycles (about 10 s on a IIe) | 10 | 2026-10-26 |
