# Benchmarks

Real programs on the built-in Apple IIe, measured headless with the release
build. 10.23 M cycles is 10 seconds on a real IIe. Re-run after compiler
changes and add a row; never edit old rows.

```sh
./build/tisple run programs/life/main.tsp --cycles 10230000 --peek <generation>
./build/tisple run programs/plasma/main.tsp --cycles 10230000 --peek <frame>
```

The counters' addresses come from `tisple build <prog> --asm` (search for the
variable's name); zero-page allocation moves them.

| Date | Commit | Program | Measure | Result |
| --- | --- | --- | --- | --- |
| 2026-10-26 | 09c9aba | life | generations in 10 M cycles | 10 |
| 2026-10-26 | f3fe505 | life | generations in 10 M cycles | 43 |
| 2026-10-26 | 73b02ee | plasma | frames in 10 s (10.23 M cycles) | 140 (14 fps) |
| 2026-10-26 | (plasma 256-entry ramps) | plasma | frames in 10 s | 177 (17.7 fps) |
| 2026-10-26 | 6d9e7a4 | life | generations in 10 s | 56 |
| 2026-10-26 | 6d9e7a4 | plasma | frames in 10 s | 184 (18.4 fps) |
| 2026-10-26 | ece08c3 (old backend) (tables from macros, byte-identical) | plasma | frames in 10 s | 177 (17.7 fps) |
| 2026-10-26 | (draw loop reads (at col x) twice, no local c) | plasma | frames in 10 s | 183 (18.3 fps) |
| 2026-10-26 | (that loop as macros.unroll x 0 40) | plasma | frames in 10 s | 201 (20.1 fps); code 481 to 1561 bytes |
| 2026-10-26 | f5b7278 (macros + gap-fix backend) | plasma | frames in 10 s | 201 (20.1 fps) |
| 2026-10-26 | (uncalled functions dropped) | life | generations in 10 s | 56; code 1680 to 1063 bytes |
| 2026-10-26 | (uncalled functions dropped) | plasma | frames in 10 s | 200 (20.0 fps; its tables moved); code 1561 to 1346 bytes |
| 2026-10-26 | (byte tables kept inside pages) | plasma | frames in 10 s | 205 (20.5 fps) |
| 2026-10-26 | (constant folding after unroll) | plasma | frames in 10 s | 205 (unrolled draw unchanged, 1129 bytes) |
| 2026-10-26 | (two table reads: second index in X, ORA table,X) | plasma | frames in 10 s | 231 (23.1 fps); code 1346 to 1191 bytes |
| 2026-10-26 | (byte expressions of loops stepped or hoisted) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-26 | db084d8 (backend items 14 to 20, main merged) | life | generations in 10 s | 56 |
| 2026-10-26 | db084d8 (backend items 14 to 20, main merged) | plasma | frames in 10 s | 243 (24.3 fps); code 1203 bytes |
| 2026-10-27 | (race item 1: step pointers, while loops in registers) | life | generations in 10 s | 56 |
| 2026-10-27 | (race item 1: step pointers, while loops in registers) | plasma | frames in 10 s | 243 (24.3 fps); code 1203 bytes |
| 2026-10-27 | (race item 1 rest: page pointers) | life | generations in 10 s | 56 |
| 2026-10-27 | (race item 1 rest: page pointers) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-27 | (race item 2: multiply by bytes) | life | generations in 10 s | 56 |
| 2026-10-27 | (race item 2: multiply by bytes) | plasma | frames in 10 s | 243 (24.3 fps); code 1200 bytes |
| 2026-10-27 | (race item 2: quarter-square multiplies in loops) | life | generations in 10 s | 56 |
| 2026-10-27 | (race item 2: quarter-square multiplies in loops) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-27 | (race item 3: while loops take outer counters registers, trip-count weights) | life | generations in 10 s | 56 |
| 2026-10-27 | (race item 3: while loops take outer counters registers, trip-count weights) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-27 | (race item 4: indexed bytes kept in registers) | life | generations in 10 s | 56 |
| 2026-10-27 | (race item 4: indexed bytes kept in registers) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-27 | (race item 5: constants of u16 indices in addresses) | life | generations in 10 s | 56 |
| 2026-10-27 | (race item 5: constants of u16 indices in addresses) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-27 | (race item 6: logic and compares by byte, CRC steps) | life | generations in 10 s | 56 |
| 2026-10-27 | (race item 6: logic and compares by byte, CRC steps) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-27 | (race item 8: count-down, rotated whiles, decided branches) | life | generations in 10 s | 56 |
| 2026-10-27 | (race item 8: count-down, rotated whiles, decided branches) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-27 | (race item 7: small loops unrolled, strlen high byte rebuilt) | life | generations in 10 s | 56 |
| 2026-10-27 | (race item 7: small loops unrolled, strlen high byte rebuilt) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-27 | (race items 8 and 10: jump threading, counters replaced by pointers) | life | generations in 10 s | 56 |
| 2026-10-27 | (race items 8 and 10: jump threading, counters replaced by pointers) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-27 | (race items 4 and 9: registers at loop tops, compares against v + 1) | life | generations in 10 s | 56 |
| 2026-10-27 | (race items 4 and 9: registers at loop tops, compares against v + 1) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-27 | (race items 10, 6, 9: bound conversions, unit steps, high bytes first) | life | generations in 10 s | 56 |
| 2026-10-27 | (race items 10, 6, 9: bound conversions, unit steps, high bytes first) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-27 | (race item 7: scans two at a time) | life | generations in 10 s | 56 |
| 2026-10-27 | (race item 7: scans two at a time) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-27 | (race item 10: constant differences for replaced counters) | life | generations in 10 s | 56 |
| 2026-10-27 | (race item 10: constant differences for replaced counters) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-27 | (race item 3: page pointers that follow a stepped variable) | life | generations in 10 s | 56 |
| 2026-10-27 | (race item 3: page pointers that follow a stepped variable) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-27 | (race item 3: invariant loads out of loops) | life | generations in 10 s | 56 |
| 2026-10-27 | (race item 3: invariant loads out of loops) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-29 | (fuzz farm run 6: loops entered by a jump) | life | generations in 10 s | 56 |
| 2026-10-29 | (fuzz farm run 6: loops entered by a jump) | plasma | frames in 10 s | 243 (24.3 fps) |
| 2026-10-29 | (mid-level passes 1-3: dead code, loop deletion) | life, plasma | generations, frames in 10 s | 56, 243 (the same, byte for byte) |
| 2026-10-29 | (mid-level passes 4-5: constants across statements, loop-invariant code motion) | life, plasma | generations, frames in 10 s | 56, 243 (the same, byte for byte) |
| 2026-10-29 | (mid-level passes 6-7: common subexpressions, facts from conditions, calls worked out at compile time) | life, plasma | generations, frames in 10 s | 56, 243 (the same, byte for byte) |
| 2026-10-29 | (backend: division early outs, signed constant divisors, multiplies by run counts) | life, plasma | generations, frames in 10 s | 56, 243 (the same, byte for byte) |
