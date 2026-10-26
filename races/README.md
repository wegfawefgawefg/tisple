# The compiler race

Tisple against cc65, LLVM-MOS and Oscar64 on the same kernels, timed in our own
cycle-exact simulator. Results and analysis: `docs/races.md`.

```sh
races/install.sh            # once: the three competitors, ~550 MB, no root
races/run.sh                # every kernel, every compiler, prints the table (~1 min)
races/run.sh --kernels sort,crc16 --compilers oscar64,tisple --variants
races/run.sh --update-doc   # rewrite the result tables in docs/races.md
races/run.sh --gaps         # tisple gaps on each Tisple kernel, on the race's inputs
```

`run.sh` uses `build/tisple` when there is one, else `build-debug/tisple`, or `$TISPLE`.
Builds and logs go to `races/out/` (ignored by git); `races/out/results.json` has every
build's cycles, bytes, objects and listing path.

## Competitors

Installed under `~/.local/share/tisple/compilers/` (or `$TISPLE_COMPILERS`), with a
version-free symlink to each. Pinned 2026-10-26, each the latest release that day.

| Compiler | Version | Source | Built |
| --- | --- | --- | --- |
| cc65 | V2.19 (prints "V2.18 - Git 5552824": the tag never bumped the string) | `https://github.com/cc65/cc65`, tag `V2.19`, commit `555282497c3ecf8b313d87d5973093af19c35bd5` | from source: `make bin`, `make -C libsrc sim6502` |
| LLVM-MOS | llvm-mos-sdk v23.4.0 (clang 24.0.0git, llvm-mos `a40207a371b6`) | `https://github.com/llvm-mos/llvm-mos-sdk/releases/download/v23.4.0/llvm-mos-linux.tar.xz` | prebuilt |
| Oscar64 | v1.32.273 | `https://github.com/drmortalwombat/oscar64`, tag `v1.32.273`, commit `9408778695b5442e755832711b89243b4f94a9ff` | from source: `make -C make compiler` |

## Settings tried

Every kernel is built with every setting below; the fastest correct build of each
compiler is its score (ties go to fewer bytes). `--variants` prints them all.

| Compiler | Target | Flags |
| --- | --- | --- |
| cc65 | `-t sim6502` startup and library, `toolchains/cc65-race.cfg` (raw binary at $6000) | `-Oirs -Cl`, `-Oirs` |
| LLVM-MOS | `mos-sim-clang -mcpu=mos6502` (the SDK's LTO default) | `-O2`, `-O3` |
| Oscar64 | `-n -tm=c64 -tf=prg`, code below $2000, bss and stacks at $6000 (`include/race.h`) | `-O2`, `-O3`, `-O3 -Ox -OM` |
| Tisple | `tisple build --origin 0x6000` | the compiler's only setting |

Each C compiler also tries both buffer modes (`races.py` writes `buffers.h` per build):
`const`, buffers as constant pointers such as `((uint8_t *)0x2000)`, so the compiler knows
the address as Tisple does with `:address`; and `extern`, buffers as extern arrays placed
by the linker (cc65 `-D`, LLVM `--defsym`), which gives LLVM's alias analysis distinct
objects. Oscar64 has no linker symbols, so it tries `striped` instead when a kernel has
arrays of multibyte elements: those arrays become `__striped` globals, the field-split
layout Tisple uses for every such array.

The CPU is the NMOS 6502 for everyone (our simulator's; LLVM's `-mcpu=mos6502`, cc65's
default `--cpu 6502`, Oscar64's default). No 65C02 instructions.

## How a kernel is timed

1. The kernel is `main` in every language, so no compiler needs to be talked out of
   inlining it, and the compilers see a whole program as they would in real use.
2. Its inputs and outputs are buffers at fixed addresses in $2000-$5FFF, which no
   contestant's code, data or stack uses (`toolchains.build` checks). The C compilers
   cannot see their contents, so nothing is folded at compile time.
3. `tisple race` (`src/race/`) loads the binary, runs it from its own entry point (each
   compiler's startup and C runtime init run as usual), and when the PC first reaches
   main writes every buffer: inputs, and a known background for outputs.
4. The clock runs from main's first instruction to its RTS, inclusive; the JSR that
   called it is not counted. The run ends when the PC is back at the caller's return
   address with the stack where it was.
5. The outputs are decoded from memory in each build's layout and compared with the
   Python reference in `kernels.py`. A wrong result does not count.
6. Bytes: every code object the kernel executed, counted whole: the kernel's own
   functions and every runtime or library routine it called (memset, multiply,
   `pushax`...), taken from each toolchain's map (cc65 modules, LLVM ELF symbols,
   Oscar64 objects, Tisple listing spans and `rt.` routines). Data tables are not counted.

## Files

- `install.sh`: reproduces the install.
- `run.sh`, `race.py`: the driver; `toolchains.py` builds and reads maps; `kernels.py` the
  kernels' buffers, inputs and references; `layout.py` packed and field-split bytes.
- `include/race.h`: the C kernels' header.
- `kernels/<name>/kernel.c` and `kernel.tsp`: the same algorithm, data and widths
  (`uint8_t` for `u8`). Other files are other ways to write it: `lib.c` through the C
  library, `index.c` without element pointers (for Oscar64's `__striped`), `unroll.tsp`
  with `macros.unroll`. Every one is raced; the table names the one that won.

## Adding a kernel

Add a `Kernel` to `kernels.py` (buffers with addresses in $2000-$5FFF, scalars at
$5F00, an input generator and a reference), then `kernels/<name>/kernel.c` and
`kernel.tsp`. Keep both idiomatic: C that a 6502 C programmer would write, Tisple as the
language docs write it.
