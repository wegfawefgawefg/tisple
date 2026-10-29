# Tisple ][

A small typed Lisp for the Apple II, with its own optimising 6502 compiler, a
built-in Apple IIe to run on, and the tools around it.

<p>
<img src="docs/images/plasma.png" width="280" alt="plasma">
<img src="docs/images/life.png" width="280" alt="Conway's Life">
<img src="docs/images/paint.png" width="280" alt="paint, drawn with the mouse">
</p>

*Plasma at 24 fps, Conway's Life, and a mouse paint program, on the built-in IIe.*

```scheme
(import apple2 a2)
(import lores)
(import tables)

; 128 + 127 sin(2 pi i / 256), computed by the compiler
(macro sine_byte (i)
    (round (+ 128 (* 127 (sin (/ (* (* 2 pi) i) 256))))))

(tables.table SINE u8 256 sine_byte)

(fn read_key () u8
    (let key u8 (io_read u8 a2.KBD))
    (& key 0x7F))
```

## Faster than the other 6502 compilers

13 kernels written in C for cc65, LLVM-MOS and Oscar64, and in Tisple, all timed
on the same cycle-exact simulator, each output checked. Cycles, lower is better;
Tisple wins every one. Details, settings and versions in
[`docs/races.md`](docs/races.md); rerun with `races/run.sh`.

| Kernel | cc65 | LLVM-MOS | Oscar64 | Tisple | vs best other |
| --- | ---: | ---: | ---: | ---: | ---: |
| 8-bit multiply | 80,903 | 101,512 | 63,165 | **29,331** | 0.46 |
| 16-bit multiply | 101,818 | 90,156 | 65,477 | **31,296** | 0.48 |
| Mandelbrot | 11,216,044 | 9,969,128 | 7,601,337 | **4,177,022** | 0.55 |
| memcpy 4 KB | 59,878 | 153,449 | 65,824 | **38,151** | 0.64 |
| fill 8 KB | 63,996 | 238,896 | 90,467 | **43,022** | 0.67 |
| Life step | 1,333,565 | 232,244 | 238,566 | **160,648** | 0.69 |
| insertion sort | 2,020,054 | 463,713 | 422,725 | **356,260** | 0.84 |
| CRC-16 | 650,111 | 257,916 | 221,527 | **188,348** | 0.85 |
| sieve | 2,285,731 | 2,510,574 | 1,258,060 | **1,086,425** | 0.86 |
| sprites | 819,546 | 117,496 | 76,125 | **65,957** | 0.87 |
| strlen | 36,153 | 186,081 | 39,103 | **31,642** | 0.88 |
| hi-res lines | 2,451,456 | 767,813 | 576,133 | **507,296** | 0.88 |
| search | 536,420 | 136,618 | 150,500 | **128,734** | 0.94 |

cc65 V2.19, llvm-mos-sdk v23.4.0, Oscar64 v1.32.273, each at its fastest
setting, plain NMOS 6502. Fill and memcpy use Tisple's `unroll` macro.

Honest status (2026-10-27): these are 13 kernels. On random programs raced
the same way, LLVM-MOS and Oscar64 still win most, because Tisple doesn't yet
delete dead work or hoist loop-invariant work (see "Random programs" in
`docs/races.md`). And the backend that set these numbers has rare miscompiles
the fuzz farm found, about 1 in 150,000 random programs; fixes are in progress.

How: whole-program compilation, no recursion (static frames shared between
functions that never run together, all in zero page where they fit), struct
and 16-bit arrays split into byte tables (`LDA field,X`), pointers stepped in
`(p),Y`, known carry and value ranges, division by constants unrolled,
quarter-square multiply tables, and a profiler and superoptimizer that rank
every wasted cycle (`tisple gaps`).

## What's in the box

- **The language.** Typed, explicit, minimal: `u8`/`i8`/`u16`/`i16`, structs,
  enums with `match`, modules, pointers, placement (`:boundary 256`), and
  macros that run at compile time (tables, sprites from ASCII art, unrolling).
  See [`docs/language.md`](docs/language.md).
- **A built-in Apple IIe.** Cycle-exact 6502 (floooh's chips), text, lo-res and
  hi-res video, speaker sound, keyboard, AppleMouse, Disk II, snapshots.
  Launches instantly with no ROM; optionally boots Apple's ROMs if you have them.
- **`tisple run game.tsp`** compiles and opens the window in milliseconds;
  headless with `--frames`, `--keys`, `--mouse`, `--screenshot`, `--wav`.
- **Self-booting disks.** `tisple build game.tsp --disk game.dsk` makes a
  `.dsk` that boots on a real Apple II with no DOS; `tisple disk` edits DOS 3.3
  images.
- **Editor support.** VS Code: semantic colours, hover types, go to
  definition, find references, outline, live errors, format on save, F5 to run.
  Neovim syntax too.
- **Correctness.** A reference interpreter and hundreds of millions of random
  programs, compiled and compared byte for byte (`scripts/fuzz_farm.sh`).

## Programs

| Program | Run |
| --- | --- |
| Plasma | `tisple run programs/plasma/main.tsp` |
| Life | `tisple run programs/life/main.tsp` |
| Snake | `tisple run programs/snake/main.tsp` |
| Paint (mouse) | `tisple run programs/paint/main.tsp` |

## Build

```sh
cmake --preset release && cmake --build --preset release   # build/tisple
cmake --preset dev && cmake --build --preset dev           # build-debug/, with tests
./build-debug/tisple_tests
```

C++20, CMake and Ninja. SDL3 for the window (optional). Design notes in
[`docs/design.md`](docs/design.md), the plan in [`docs/roadmap.md`](docs/roadmap.md),
speed history in [`docs/benchmarks.md`](docs/benchmarks.md).

## License

GPL-3.0-or-later (`LICENSE`), with a runtime exception: the startup and runtime
code the compiler puts into your programs is also MIT, so programs built with
Tisple can use any license. See `COPYRIGHT`. Code under `third_party/` keeps
its own license.
