# Mid-level optimiser

Planned 2026-10-29. The work queue after the round-4 miscompiles are fixed.

## Why

The random-program race (186,825 programs on stevejobs, `docs/races.md`,
"Random programs"): Tisple loses 94% to the fastest of LLVM-MOS and Oscar64,
geomean 3.1x. Not codegen: the losses are work the C compilers delete or hoist
and Tisple runs as written. Worst case, seed 21258: Tisple 264,845 cycles,
Oscar64 19. Its `run` ends in three nested loops whose body is an empty counting
`while` and a `let` nobody reads; Oscar64 deletes all three loops. Division is the
worst family overall (45% of programs, geomean 4.05x).

Regenerate any race program locally with `tisple race-gen --seed S --print`.

## Where it runs

On the checked program (`src/check/program.hpp`), after expansion and before
code generation: tree-to-tree passes in a new `src/opt/`, each one small, run to
a fixed point. The backend's own peephole and register work stay as they are.
The reference interpreter runs the unoptimised program, so every pass is checked
by the existing differential tests and the fuzz farm.

## Facts the language already gives (and that C compilers must guess)

- No recursion; whole program visible: exact call graphs and effect summaries.
- Division and modulo by zero are defined (all ones, or the dividend), never a
  trap: hoisting or speculating a division is always safe.
- Pointer provenance: an `addr_of` pointer reaches only its variable, an `as`
  pointer only undeclared memory.
- `io_read`, `io_write` and `jsr` are the only effects outside memory; they are
  never removed, merged or moved.

## Passes, in order of expected cycles won

1. **Effect summaries.** Per function: which globals, arrays and pointer
   targets it may read and write, whether it does I/O or `jsr`. Pure functions
   are known.
2. **Dead code.** Locals and `let`s never read; stores to locals overwritten
   before any read; self-assignments `(set g g)`; expression statements with no
   effect. Globals: a store dead if no read can see it before the next store or
   program end, within what effect summaries prove.
3. **Loop deletion.** A loop whose body has no effect outside itself and whose
   results are dead is removed, if it provably terminates: every `for`
   terminates; a `while` terminates when a counter moves monotonically towards
   its bound. Non-terminating loops are kept. Settled 2026-10-29: timing goes
   through the explicit `(wait_cycles n)` primitive (docs/language.md), never
   through effect-free loops.
4. **Loop-invariant code motion.** Pure expressions whose inputs the loop never
   writes move to before the loop (division first; always safe here). Loops
   that may run zero times still hoist safely because the expressions are pure
   and total.
5. **Constant propagation and folding across statements.** Sparse conditional
   constant propagation over locals; globals between writes; branches whose
   condition becomes constant are cut; `match` on a constant picks its clause.
6. **Common subexpressions.** Local value numbering inside blocks; across
   blocks where dominance is simple (the tree makes this cheap).
7. **Compile-time evaluation.** A pure function called with constant arguments
   runs in the interpreter at compile time and becomes a constant (the macro
   machinery already does this for macros).
8. **Value ranges** (extend the backend's): narrow i16/u16 work to bytes when the
   range fits; this is what made Oscar64 keep `lib/hgr`'s `y` in X.
9. **Division by variables.** A u8 / u8 fast path; hoisting loop-invariant
   divisors' reciprocal tables when a divisor repeats.

## Flags

- `-O0` no optimisation (debugging); `-O1` default, every fast pass; `-O2`
  release, adds slow search (solver superoptimisation, exact allocation) with a
  persistent cache keyed by code window; `-Os` size instead of cycles;
  `--cpu nmos|65c02`.
- Developer: `--opt-bisect N` (apply only the first N transformations, for
  bisecting miscompiles; build this first), `--no-pass NAME`, `--remarks` (what
  was not optimised and why, with the source line).
- No per-optimisation user flags and no in-code annotations until a real
  program needs them (minimal rule).

## Measure

- After each pass: the random race on 2,000 local seeds (`races/random.sh
  --seed 1 --count 2000 --jobs 6`), the kernel race, `tisple_tests` kernels, Life
  and plasma. Record in `docs/benchmarks.md` and `docs/dogfood.md`.
- Then a long random race on stevejobs (fix the macOS host check first:
  `races/` host builds hang and time out on many seeds there).
- Correctness: the interpreter oracle, targeted tests per pass, fuzz shapes for
  each pattern, then a correctness farm run on stevejobs after merging.

## Goal

Win the random race outright against every competitor, not only the kernels,
and keep every kernel win.
