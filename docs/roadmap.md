# Tisple ][ roadmap

Set 2026-10-26 (owner): at the end, a body of real programs written in Tisple,
running on an accurate Apple II, fast, with tests proving the output is as good
as it can be. Then use it to make software and see how fast it can go.

## 1. A built-in Apple II

`tisple run game.tsp` compiles, loads the binary straight into RAM and runs it
in an Apple IIe model built around our cycle-exact CPU (`src/emu/`). No Apple
ROM and no disk boot: our startup code needs neither, so launch is instant and
there is nothing copyrighted to ship.

- Window mode: real 1.023 MHz speed, keyboard, speaker audio, close and done.
- Headless mode: as fast as the host allows (~160x real time), scripted keys
  at given cycles, screenshots (PNG) and speaker capture (WAV) at given
  cycles, for tests.
- Video: text (our own Apple-like font), lo-res, hi-res with NTSC color
  artifacts, mixed mode, both pages.
- Final cross-check, later: the same binaries in MAME's apple2e with ROMs the
  owner supplies.

## 2. Programs

Each is also a benchmark. Rough order:

1. Hi-res sprite demo (pre-shifted sprites, XOR drawing).
2. Snake (lo-res), Breakout (hi-res).
3. Conway's Life.
4. Speaker music player.
5. Line drawing, filled shapes.
6. Fixed-point Mandelbrot.
7. Tetris.
8. 3D wireframe.
9. Raycaster.

## 3. Proving the output is fast

1. Exact-output tests: each common pattern asserts its instructions, e.g.
   `(at table i)` with `i : u8` is `LDA table,X` and nothing more.
2. Superoptimizer proofs for small cases: an exhaustive search over short
   instruction sequences, checked for equivalence on the simulator, finds the
   shortest; tests assert the compiler matches it. Feasible up to about five
   instructions.
3. Cycle and size budgets on whole kernels, compared with the best known
   hand-written assembly and with cc65, LLVM-MOS and Oscar64. A change that
   makes a kernel slower fails the build.

"Optimal" is provable only for small pieces; for whole programs the bar is the
best hand-written assembly.

## 4. Use it

Write software with it, measure, and keep pushing the backend.
