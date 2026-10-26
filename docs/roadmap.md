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
- Borrow, don't rewrite (owner, 2026-10-26): Tisple is GPL-3.0-or-later, so
  machine code may be adapted from AppleWin (GPL-2.0-or-later; its NTSC.cpp
  is the reference for hi-res colour), izapple2 (GPL-3.0), Mii and Clock
  Signal (MIT), with attribution under third_party/. No Apple ROM or
  ROM-derived data (the character font included) is ever committed. Code the
  compiler emits into programs stays original (runtime exception).
- Why a thin machine of our own at all: full emulators boot through Apple's
  ROM from a disk image, which means copyrighted ROMs and seconds of boot.
  Our programs never touch ROM or disk, so the part we need is small:
  memory, about 30 soft switches, keyboard, speaker, video.
- Real-machine check, a separate slow step: a disk image of the program run
  in Mii or MAME with real ROMs kept locally, never committed.

### Accurate and instant (owner, 2026-10-26)

The goal is perfectly accurate emulation with speed, snapshots and disk
images to fiddle with during development, then real hardware.

1. One machine, two starts: direct load (no ROM, instant, unit tests) and
   disk boot. No Apple code is needed for either (owner, 2026-10-26):
   - Our disks boot themselves: `tisple disk` writes our own loader into
     track 0 sector 0, which reads the program in and jumps to it. No DOS. A
     real Apple II boots it with its own ROM.
   - The emulator's Disk II card (adapted from AppleWin) gets our own boot ROM
     that acts like Apple's: read track 0 sector 0 to `$0800`, jump to
     `$0801`.
   - A real Apple ROM (owner-supplied, never committed) is only for running
     Apple's own software (DOS 3.3, ProDOS, Applesoft, a2audit) and for
     accuracy comparisons.
2. Snapshots: the whole machine state is plain data (a few hundred KB). Boot
   DOS once, snapshot at the prompt, restore in microseconds for every run.
   Also for debugging: snapshot before a bug and rerun it.
3. Disk tooling: `tisple disk` creates DOS 3.3 and ProDOS images and adds,
   lists and extracts files; `tisple build` can write onto an image.
4. Accuracy proved: Zellyn Hunter's a2audit disk passes in our machine; the
   same programs in AppleWin or MAME give the same memory and screen.
5. Real hardware: the same disk images through a FloppyEmu-style drive or
   ADTPro.

Status (2026-10-26, see `docs/disks.md`): the Disk II card (bit cells, two
drives, reads, writes, write-back), `.dsk`/`.do`/`.po`/`.nib`, our boot ROM,
self-booting disks (`tisple build --disk`, `tisple run --disk`,
`tisple disk boot`), DOS 3.3 file commands and snapshots with disks are in.
Self-booting disks boot with Apple's `Apple2e.rom` and `DISK2.rom` in the
emulator; a2audit's language card and soft switch tests pass. Still to do:
ProDOS file commands, aux memory (a2audit skips its test), real hardware.

### Mouse (owner, 2026-10-26)

AppleMouse II card (slot 4 on a IIe; built in on a IIc). Programs call the
card's firmware routines rather than the hardware.

1. Emulate the card, borrowing AppleWin's mouse card code (GPL), with the real
   `MouseInterface.rom` kept locally like the other ROMs. The window passes
   the host mouse and buttons through.
2. Firmware calls from Tisple: a way to jump into ROM with A, X and Y set and
   read them back (the planned ROM-call / inline-assembly feature). The mouse
   is its first user.
3. `lib/mouse.tsp` wraps it. On real hardware the program needs a mouse card
   or a IIc.

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
