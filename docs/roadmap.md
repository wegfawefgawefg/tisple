# Tisple ][ roadmap

Set 2026-10-26 (owner): at the end, a body of real programs written in Tisple,
running on an accurate Apple II, fast, with tests proving the output is as good
as it can be. Then use it to make software and see how fast it can go.

## 1. A built-in Apple II

`tisple run game.tsp` compiles, loads the binary straight into RAM and runs it
in an Apple IIe model built around our cycle-exact CPU (`src/emu/`). No Apple
ROM: our startup code needs none, so launch is instant and there is nothing
copyrighted to ship. (Disk boots came later, with our own boot ROM: "Accurate
and instant" below.)

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
emulator; a2audit's language card and soft switch tests pass. 2026-10-27: the
extended 80-column card (aux memory, 80 columns, double-res, MouseText) is in,
a2audit finds 128K and its aux memory tests pass, and disks boot programs in
segments across main memory, the language card and aux memory. Still to do:
ProDOS file commands, real hardware.

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

Status (2026-10-26): all three are in, with `programs/paint` as the demo.

- The card (`src/machine/mouse.hpp` has the interface it emulates) is in slot 4
  for `tisple machine` and `tisple run`, with our own firmware
  (`src/machine/mouse_rom.cpp`): SETMOUSE, SERVEMOUSE, READMOUSE, CLEARMOUSE,
  POSMOUSE, CLAMPMOUSE, HOMEMOUSE, INITMOUSE through the offsets at
  `$Cn12-$Cn19`, the identification bytes, and the screen holes. Polled mode
  only: no interrupts. `--mouse-rom MouseInterface.rom` uses Apple's firmware
  instead, against an emulated 6821 and AppleWin's model of the 6805's
  protocol; it calls the monitor's IORTS, so it also needs `--rom Apple2e.rom`.
  `tests/test_machine_mouse.cpp` runs the same checks on both firmwares.
- `--mouse "cycle:x,y,button;..."` scripts it for headless runs; cycles take
  the `f` frame suffix like `--keys`.
- The window: the pointer's motion over the window moves the mouse, one hi-res
  pixel per two screen pixels at scale 1. A click into the window captures the
  pointer (hidden and held, that click only captures); then motion and the
  left button go to the card until Esc (which the Apple does not see) or losing
  focus releases it.
- `(jsr addr (a v) (x v) (y v))` calls machine code (`docs/language.md`,
  Machine code); `lib/mouse.tsp` finds the card in slots 1-7 and calls it with
  X = `$Cn` and Y = `$n0`. `lib/hgr.tsp` has the hi-res tables, plot, flip and
  line the paint program draws with.

```sh
tisple run programs/paint/main.tsp --window      # click in, hold the button to draw
tisple run programs/paint/main.tsp --window \
    --rom ~/.local/share/tisple/roms/Apple2e.rom \
    --mouse-rom ~/.local/share/tisple/roms/MouseInterface.rom
```

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

Status (2026-10-30): in `programs/`: plasma, Life, Snake, paint (mouse, hi-res
lines) and an 80-column text viewer that fills main, aux and language card
memory; `examples/mockingboard_demo.tsp` plays music. Mandelbrot, sprites and
hi-res lines are race kernels. Catwiz, a dice roguelike for the 128K IIe, is a
game of its own (github.com/wegfawefgawefg/catwiz).

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

Status (2026-10-26): the race against the three C compilers is in (`races/`,
`docs/races.md`): 13 kernels, timed and checked in our simulator. Tisple wins 1, Oscar64
8, cc65 3 (with its assembly library), LLVM-MOS 1; `docs/races.md` ranks the backend
work that closes the gap. 2026-10-27: Tisple wins all 13. The random race
(`races/random.sh`, 2000 generated programs) went from a geomean of 3.1 times the
fastest C compiler (2026-10-27) to 0.809, faster on 69.1% of them (2026-10-30),
through the mid-level passes and backend work logged in docs/dogfood.md. No build-failing budgets yet.

"Optimal" is provable only for small pieces; for whole programs the bar is the
best hand-written assembly.

## 4. Use it

Write software with it, measure, and keep pushing the backend. Catwiz is the
first big one (docs/dogfood.md logs what it needed from the compiler).

Open bugs:

- lib/dhgr runs from the aux language card (dhgr.FROM_CARD) with the rom switched in for
  reading: a second such call breaks the card's runs for every call after it and loses
  interrupt ticks (found 2026-10-31 adding cutout draws to tests/test_dhgr_runs.cpp; with the
  card's ram in, as a program in the language card runs, it is fine). lib/auxcard's rom path
  (the guard vector at $FFFE, the bank put back after) is the place to look.
