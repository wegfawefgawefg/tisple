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

## 5. Libraries: what Apple II programs reach for

Survey 2026-11-01 (owner: make Tisple a general Apple II language, not one game's). What
programmers have always called (the monitor and Applesoft routines, cc65's Apple II drivers,
the classic toolkits and game code) and what today's scene runs on, against what `lib/` has.

### What Tisple has

Keyboard (`keys`), the speaker as clicks and buzzes (`speaker`), the Mockingboard with a
tracker-style player and effects (`mockingboard`), the AppleMouse (`mouse`), 40- and 80-column
text (`text40`, `text80`), lo-res, hi-res plot/flip/line (`hgr`), double hi-res with sprites
and page flipping (`dhgr`), aux memory and the language card (`auxmem`, `:in aux`, bank 1
code), packed data (`unpack`), tables read from the boot disk while running (`disk`), frame
timing on a IIe (`timing`), random numbers, and interrupt handlers in the language.

### What programs reach for, and where we stand

- **The monitor and Applesoft.** Programs of every era call `PREAD` (`$FB1E`, a paddle),
  `WAIT` (`$FCA8`), `BELL` (`$FF3A`), `HOME`/`COUT`/`VTAB` for text, `PRBYTE`/`PRNTAX` for hex,
  and Applesoft's `HGR`, `HCOLOR`, `HPLOT`, `DRAW`/`XDRAW` (shape tables) and `PDL`/`PEEK
  (-16287)` for paddles and buttons. Tisple has text, delays and lines; it lacks paddles and
  buttons, coloured hi-res drawing (HCOLOR), shape tables, number printing and a bell. The
  ROM listings are on 6502disassembly.com.
- **cc65's Apple II target.** Its drivers are what C programmers get on day one: a joystick
  driver (two analog sticks, both axes read in one timed loop), the AppleMouse, Super Serial
  and IIgs serial, TGI graphics for lo-res (with `tgi_apple2_mix` for the four text lines)
  and hi-res, the aux-memory extended memory driver, `conio` with MouseText box drawing
  (`mt_chline`, `CH_ULCORNER`...), full ProDOS 8 file I/O (`fopen`, `_filetype`, direct
  block I/O through `dio_open`), `get_ostype` (II, II+, IIe, enhanced IIe, IIc variants, IIgs),
  `waitvsync`, `videomode` (40/80) and `beep`. Of those Tisple has the mouse, aux memory and
  text; it lacks the joystick, machine identification, mixed graphics and text, ProDOS files,
  serial and MouseText helpers.
- **Machine identification.** Apple's own method (Miscellaneous Technical Notes #7 and #2):
  the ROM bytes at `$FBB3`, `$FBC0`, `$FBBF` (and `$FB1E`, `$FBDD`, `$FBBE` for the rarer
  ones), `SEC`/`JSR $FE1F` for a IIgs, and on a IIe a probe of aux memory run from a page RAMRD
  does not switch (the sparse `$0800`/`$0C00` test finds the 1K 80-column card). Libraries need
  it themselves: the vertical blank reads differently on the IIc and IIgs, MouseText exists
  only on enhanced machines, and only some have 128K.
- **Game code.** Paddle and joystick reads with the two buttons (open- and closed-apple on a
  IIe) in nearly every action game; two-voice speaker music (Paul Lutus's Electric Duet, still
  "probably the most popular music format in the Apple II world") and sampled-speaker
  effects; hi-res sprites pre-shifted seven ways; text drawn on the hi-res screen; high-score
  tables saved back to the game's own disk with RWTS.
- **Classic toolkits.** MouseText menus and windows (MouseDesk, the IIc utilities, and today
  a2stuff's Apple II DeskTop), the 80-column firmware's text windows, Applesoft's shape
  tables, the Beagle Bros utilities; double lo-res (80 x 48) and double hi-res for the 128K
  machines, which only the IIc and IIgs firmware supports from BASIC.
- **Today's scene.** Storage is mostly flash: the Floppy Emu (5.25" images on any II, and
  SmartPort hard disks up to 32 MB on the IIc, IIgs or a Yellowstone/Liron controller), the
  CFFA3000 (floppy images and 32 MB ProDOS volumes), the MicroDrive/Turbo (ProDOS volumes over
  DMA, fast) and BOOTI; all but the Floppy Emu's 5.25" mode are ProDOS block devices, so
  ProDOS file I/O is how a program reaches them. Memory: RamWorks-style aux banks. Networking:
  Uthernet II and FujiNet (KansasFest 2025 had a FujiNet talk). KansasFest's talks and HackFest
  entries are mostly cc65 and assembly, ProDOS tools and demos.

### The gaps, ranked

Want: how much the programs above lean on it. Rough cost in a Tisple library.

| # | Gap | Want | Cost | Round |
| --- | --- | --- | --- | --- |
| 1 | Joystick and paddles: both axes in one timed read, the three buttons | every action game | small (a 6502 loop, a lib) | 1 |
| 2 | Saving to the boot disk: high scores, saves, settings | most games | medium (a sector writer, two copies) | 1 |
| 3 | Machine detection: model, 128K, 65C02, the cards in the slots | every library that differs by machine | small | 1 |
| 4 | Numbers as text: `u8`/`u16`/`i16` in decimal and hex on the text screens | every program with a score or a debug line | small | not yet |
| 5 | Text on the graphics screens (a hi-res font, HRCG-style) | most hi-res games | small to medium | not yet |
| 6 | ProDOS 8 files and block devices (the CFFA3000, MicroDrive, Floppy Emu SmartPort, BOOTI), 800K and hard disk images | today's real machines; tools | large | approved, later |
| 7 | Colour hi-res drawing: HCOLOR plots and lines, fills, shape tables, pre-shifted sprites | most hi-res games | medium | not yet |
| 8 | Speaker music in two voices (Electric Duet style) and a speaker effects kit | most games without a Mockingboard | medium | not yet |
| 9 | Mixed graphics and text, double lo-res, MouseText menus and boxes | menus, status lines, 128K games | small | 1 |
| 10 | The 65C02 target (enhanced IIe, IIc) | speed and size on the owner's machines | large (backend) | built 2026-11-01 |
| 11 | Frame timing on the IIc and IIgs (their vertical blank reads differ) | anything synced to the screen | small, needs 3 | not yet |
| 12 | A clock (ProDOS's, the No-Slot Clock, ThunderClock) | tools, saves with dates | small | not yet |
| 13 | Serial and printer (Super Serial Card, the IIc ports) | tools, ADTPro-style transfers | medium | approved, later |
| 14 | RAM expansion (RamWorks banks, slot RAM cards) | big programs, demos | medium | approved, later |
| 15 | Mockingboard speech (SC-01, SSI-263) | a few games | medium (the emulator first) | approved, later |
| 16 | Networking (Uthernet II, FujiNet) | a growing niche | large | not yet |

Two rank above parts of round 1 and were not swapped in: numbers as text (4) and hi-res text
(5) are wanted by more programs than double lo-res and MouseText (9), and both are cheap.

Sources: [cc65 apple2enh target](https://cc65.github.io/doc/apple2enh.html),
[cc65 apple2.h](https://github.com/cc65/cc65/blob/master/include/apple2.h),
[cc65 wiki, Apple II joystick](https://github.com/cc65/wiki/wiki/Apple-II-12.-Joystick),
[Apple II Misc. Technical Note #7, family identification](https://prodos8.com/docs/technote/misc/07/),
[Misc. Technical Note #2, identification routines](https://prodos8.com/docs/technote/misc/02/),
[ProDOS 8 Technical Reference, MLI calls](https://prodos8.com/docs/techref/calls-to-the-mli/),
[Apple II ROM disassemblies](https://6502disassembly.com/a2-rom/),
[Electric Duet](https://arachnoid.com/electric_duet/) and
[Making the Apple II sing](https://www.xtof.info/blog/?p=807),
[MouseText](https://en.wikipedia.org/wiki/MouseText),
[Apple II graphics](https://en.wikipedia.org/wiki/Apple_II_graphics),
[Apple II DeskTop](https://github.com/a2stuff/a2d),
[Floppy Emu](https://www.bigmessowires.com/floppy-emu/),
[Floppy Emu vs CFFA3000/BOOTI](https://www.applefritter.com/node/32169),
[KansasFest 2025 videos](https://a2central.com/2025/07/kansasfest-2025-video-playlist/),
[FujiNet](https://fujinet.online/).

### Round 1 (2026-11-01)

Built: 1, 2, 3 and 9.

- 1: `lib/joystick` ([`hardware.md`](hardware.md)): both axes from one trigger of the timers,
  PREAD's single paddle, the three buttons; the emulator's `--joystick`, a game controller in
  the window, stick and button keys on the phone pad.
- 2: `lib/save` ([`disks.md`](disks.md), Saves): our own sector writer, two copies, tested
  through 93 power cuts; `--write-back` for `tisple run --disk`; the phone player keeps saves.
- 3: `lib/machine` ([`hardware.md`](hardware.md)): model (IIgs included), 64K/128K, 65C02,
  each slot's card; the emulator's `--model` and `--64k`.
- 9: `lib/dlores`, `lores.mixed`, `hgr.mixed`, `text40.window`/`text80.window`,
  `lib/mousetext` ([`hardware.md`](hardware.md)).

Next by the ranking: numbers as text (4) and text on the graphics screens (5), both small;
then ProDOS files (6) and the 65C02 target (10), already approved. Frame timing on the IIc and
IIgs (11) now has lib/machine to choose by.

### The 65C02 (2026-11-01)

Built: 10, with the emulator first, since the owner's 65C02 machines are out of reach.

- The cpu: our own 65C02 core beside floooh's NMOS one (`src/emu/cpu65c02*.cpp`), bus-cycle
  exact, proved by SingleStepTests' synertek65c02 set, Klaus Dormann's functional, 65C02 and
  decimal tests and interrupt timing equal to the NMOS core's (docs/design.md, Testing).
- The machine: `--cpu 6502|65c02`, and the model's cpu by default: the enhanced IIe (the
  default model), IIc and IIgs on the 65C02, so `machine.cmos` reads true; Apple2e_Enhanced.rom
  boots, passes a2audit and boots our disks on it (docs/hardware.md).
- The target: `--cpu 65c02` / `(cpu "65c02")`: STZ, BRA, `(zp)`, INC A/DEC A, PHX/PLX, TSB/TRB,
  `JMP (table,X)` jump tables and `JMP (pointer)` dispatchers; the superoptimizer and
  `tisple gaps --cpu 65c02` know the new instructions. The 6502 target's bytes did not change.
- Tests: TISPLE_TEST_CPU=65c02 runs the whole suite for the 65C02 (clean); the fuzzer builds
  a seed in four for the other chip and has a jump-table generator built for both
  (tests/fuzz_gen_tables.cpp); about 108,000 programs fuzzed on the 65C02 over the round
  (48,773 on its last code), no mismatch.
- Measured (docs/races.md, "The 65C02"): Tisple 1.0% fewer cycles and 2.4% fewer bytes on
  random programs, kernels up to 1.3%, the bytecode VM 8.8%; Catwiz 2.75% smaller, its waiting
  pass 1.5% faster. dogfood.md 30-34 has what is left.

Choices for the owner, not made: how Catwiz should ship (below), and whether the generator
should plan with the 65C02's instructions (dogfood 30).

How Catwiz could ship, laid out. It needs 128K and double hi-res, so its machines are a IIe
with the extended 80-column card (enhanced or not), a IIc, a IIc Plus or a IIgs; all but the
unenhanced IIe have a 65C02 (the IIgs's 65816 runs 65C02 code). The 65C02 build is 954 bytes
smaller, its waiting pass 1.5% faster, a turn 0.75%; it still needs squeeze 10.

1. 65C02 only: one disk, `(cpu "65c02")` in tisple.project. An unenhanced IIe would crash on
   the first 65C02 instruction, so a check must run before any: the boot loader is 6502 code
   and could test the cpu (the decimal `99 + 1` test lib/machine uses) and print a line
   instead of starting the game. That check does not exist yet.
2. Two disks, catwiz.dsk and catwiz65.dsk, from the same source by two builds (`--cpu 65c02
   --disk build/catwiz65.dsk`): each the best its chip gets; two files to test and ship, and
   the same boot check for the 65C02 one put on an unenhanced IIe.
3. One disk that chooses at boot: the loader tests the cpu and loads the matching code. Both
   images do not fit one 5.25" disk with the art (the 6502 build boots 70,813 bytes from
   tracks 1-23 and the tables fill tracks 28-34, leaving 64 sectors, 16 KB; the second image's code
   alone is 34 KB). It would need the two builds linked to the same data addresses so only
   code differs, and room: a 3.5" (800K) or ProDOS image, which the roadmap has as 6.
