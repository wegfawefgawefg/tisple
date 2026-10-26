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
