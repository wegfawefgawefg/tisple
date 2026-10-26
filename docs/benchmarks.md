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
