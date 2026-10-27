# The compiler race

Tisple against cc65, LLVM-MOS and Oscar64 on the same kernels, timed in our own
cycle-exact simulator. Results and analysis: `docs/races.md`. The same race on random
programs, millions of them if there is time, is under "Random programs" below.

```sh
races/install.sh            # once: the three competitors, ~550 MB, no root, Linux or macOS
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
| LLVM-MOS | llvm-mos-sdk v23.4.0 (clang 24.0.0git, llvm-mos `a40207a371b6`) | `https://github.com/llvm-mos/llvm-mos-sdk/releases/download/v23.4.0/llvm-mos-linux.tar.xz` (`llvm-mos-macos.tar.xz` on macOS, a universal arm64 and x86-64 build); `install.sh` checks each one's SHA-256 | prebuilt |
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

## Random programs

The kernels are 13 programs written for the race. The random race takes the fuzz
generator's programs instead (`tests/fuzz_gen*.cpp`, the ones the backend is fuzzed with),
writes each one in C too, and races all four compilers on every one: thousands of programs
nobody tuned, to find every shape where a C compiler beats Tisple. Results:
`docs/races.md`, "Random programs".

```sh
races/random.sh --seed 1 --count 2000 --jobs 6        # race seeds 1..2000 (~6 min)
races/random.sh --report                              # summary, the 20 worst rebuilt
races/random.sh --report --worst 50 --update-doc      # and into docs/races.md
races/random.sh --one 285                             # one seed, kept in out/random/one/285
build/tisple race-gen --seed 285 --print              # the seed's Tisple; --print-c its C
```

### One program

1. `tisple race-gen --seed S --out DIR` (`src/race/race_gen_command.cpp`) writes
   `DIR/S/t.tsp`: the generator's program for S in race mode (`GenerateRaceProgram`),
   which leaves out what C cannot say the same way (I/O) and adds a tail. The generated main
   is `t.run`; a new main calls `t.init`, then `run` (keeping its value in `rv`), then
   `t.checksum`, which folds every global, element by element in declaration order, into a
   u16 at $BF00. Outside race mode the generator's programs are byte for byte what they were
   (`tests/test_race_c.cpp` holds a hash of seeds 1 to 300).
2. Initial values go through `t.init`, never initializers, and each is the value plus
   `opaque`, a u16 at $BC00 the race writes as 0 when main is entered: LLVM folds a global
   it can see is only ever one constant, as it would fold the kernels' inputs, so no
   compiler may see a start value.
3. It checks the program, writes `t.c` (`src/race/race_c.hpp`: the same program in C,
   exact widths, Tisple's wrapping, shift and division-by-zero results spelled out, left to
   right evaluation kept with temporaries), and runs main in the interpreter: `info.json`
   has its checksum, its steps, the features `t.run` uses and the same weighed by how often
   each ran (the shape signature below). Programs past 3M interpreter steps are skipped.
4. Each compiler builds it at its best kernel-race setting, one setting each:

   | Compiler | Build |
   | --- | --- |
   | cc65 | `cl65 -t sim6502 -C toolchains/cc65-random.cfg -Oirs -Cl` (code, data and stack in $0800-$B7FF) |
   | LLVM-MOS | `mos-sim-clang -mcpu=mos6502 -O2` |
   | Oscar64 | `oscar64 -n -tm=c64 -tf=prg -O2`, its default layout |
   | Tisple | `tisple build t.tsp --origin 0x4000` (above the hi-res page Tisple keeps clear) |

5. `tisple race` runs each binary from its own startup and times from `t.init`'s return
   (`--after`) to `t.checksum`'s first instruction (`--stop`): the program's work, not the
   setting up or the checksum. Then it runs on to main's return, and the checksum at $BF00
   (`--word`) must be the interpreter's. Bytes are the code objects that ran while the clock
   ran (functions, cc65's cut at its function labels, and library routines).
6. A compiler whose checksum is wrong, or that fails to build or run the program, is a bug
   in that compiler, kept in `out/random/bugs/S/` with every build. When a C compiler is
   wrong the C itself is checked first: `t.c` is built for the host with
   `-fsanitize=address,undefined` (`include/random.h`, `RACE_HOST`) and run; if that
   disagrees with the interpreter the bug is ours, labelled `c-writer`, and no C compiler is
   blamed. `--host` checks every program that way, not only the ones with a mismatch.

The summary (`random_report.py`) has win, loss and tie rates against the fastest correct
C compiler, the distribution of Tisple's ratio, each compiler's geometric means, and the
losses by shape: a program's shape is the share of `t.run`'s work in each feature family
(divide, multiply, 16-bit +-, u8 index, ...), each feature weighed by how often the
interpreter ran it times a rough 6502 cost. `--report` also rebuilds the K worst losses
into `out/random/worst/<rank>-<seed>/`: `t.tsp`, `t.c`, Tisple's listing and the fastest
compiler's.

### Shards and long runs

A run is deterministic: a seed is the same program, builds and cycles everywhere (the
generator uses only its own xorshift streams). `--seed S --count N` is a shard; its results
go to `DIR/results-S-E.jsonl`, one line a seed, sorted when the shard ends. Rerunning a
shard skips the seeds it already has, so a stopped run resumes. Shards with different seed
ranges can share an `--out` directory and run at the same time; `--report` reads every
`results-*.jsonl` there. A seed takes about 0.45 s of CPU (four builds, four simulated
runs, the interpreter), so 14 cores do about a million in 9 to 10 hours. A results line is
about 600 bytes.

### On the Mac mini (macOS ARM, 14 cores, no admin)

From the repository here, on the branch to race, push a source export:

```sh
git archive --format=tar --prefix=tisple-race/ -o /tmp/tisple-race.tar HEAD
scp /tmp/tisple-race.tar mini:~/tisple-race.tar
ssh mini tar -xf tisple-race.tar
```

There (`ssh mini`, then):

```sh
cd ~/tisple-race
races/install.sh                      # once: ~10 min, builds cc65 and Oscar64 with Apple clang
scripts/build_plain.sh 12             # build/tisple without CMake; with CMake and Ninja:
                                      # cmake --preset release && cmake --build --preset release
build/tisple race-gen --seed 7 --print-c | head -3
races/random.sh --one 7 --out ~/tisple-race-out/check   # all four agree: "bugs": []
mkdir -p ~/tisple-race-out
nohup nice -n 10 races/random.sh --seed 1 --count 1000000 --jobs 12 \
    --out ~/tisple-race-out/run1 > ~/tisple-race-out/run1.log 2>&1 &
```

`--jobs 12` leaves two cores for the machine's owner. Progress: `tail ~/tisple-race-out/
run1.log` (a line every 500 seeds, and one per bug) and `wc -l ~/tisple-race-out/run1/
results-*.jsonl`. To resume after a stop or a reboot, run the same `nohup` line again.
More machines, or a second run beside the first: give each a different seed range (`--seed
1000001 --count 1000000`) and the same or another `--out`. When it is done:

```sh
races/random.sh --report --out ~/tisple-race-out/run1 --worst 50 > ~/tisple-race-out/run1.md
tar -czf tisple-race-out.tgz -C ~ tisple-race-out
```

Bring `tisple-race-out.tgz` back (`scp mini:~/tisple-race-out.tgz .`), unpack it under
`races/out/`, and `races/random.sh --report --out races/out/tisple-race-out/run1 --worst 50
--update-doc` writes the summary into `docs/races.md` (the worst losses are rebuilt here, so
their listings match this checkout).

### Files

- `random.sh`, `random.py`: the driver; `random_build.py` builds one program with one
  compiler; `random_report.py` the summary.
- `include/random.h`: the generated C's header (`RACE_NOINLINE`, `RACE_STATIC`,
  `RACE_FIXED`, the host build).
- `toolchains/cc65-random.cfg`: cc65's memory map for these programs.
- In C++: `src/race/race_c*.cpp` (Tisple as C), `src/race/race_gen_command.cpp` and
  `race_shape.cpp` (`tisple race-gen`, the shape signature), `tests/fuzz_gen_race.cpp`
  (race mode's tail), `src/race/race.cpp` (`--after`, `--stop`, `--word`).
