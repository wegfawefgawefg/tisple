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

## Landed (2026-10-29)

Passes 1 to 3, in `src/opt` (`opt.hpp` has the order and the rules), run by
`CompileProgram` on its copy of the program after constant folding:

- `summary.cpp`: per function, globals read and written, pointer loads and
  stores, I/O, jsr, and whether every loop provably ends (`terminate.cpp`).
- `live.cpp`: liveness of each function's locals, params and plain globals
  (no address taken, no `:address`, not in aux). Every global is live when a
  function returns; a call reads what its summary says; I/O, jsr, a loop that
  may never end and a call that may not return read every global (memory can
  be looked at while the program runs, as `--peek` does).
- `dead.cpp`: lets and stores nobody reads (an effectful value stays as a
  statement), `(set x x)` outside `:address` lets, pure statements, calls to
  pure functions that end.
- `loops.cpp`: a while or for whose writes are all dead after it and that
  provably ends: every for; a while whose counter's one write is an unwrapping
  step towards its bound on every pass (`(< i K)` with `(+ i c)` and
  `K - 1 + c` within the type, `>` down likewise, `!=` with an odd step).
- Nodes the passes do not know count as every effect (wait_cycles is safe).
- Flags `-O0`, `-O1` (default), `--opt-bisect N` (prints "N of M" and the
  last transformation), `--no-pass dead|loops`. Results: dogfood.md 39.

Passes 4 and 5 (2026-10-29), run after dead and loops in each round, then
constant folding (`fold.cpp`) again so new literals join their chains:

- `consts*.cpp`: a forward walk per function over its tracked integer, bool
  and enum variables (live.cpp's set), following only the arms of an if,
  match, and/or or while it can decide; loops to a fixed point, breaks and
  continues carry their values. Calls forget the globals their summary
  writes; jsr, io_write and unknown nodes forget all. Rewrites: a pure value
  with one known value becomes a literal (const array reads, division by
  zero's defined result, `x * 0`, shifts by the width, `(- x x)`, `(% x x)`,
  `(= x x)` on steady operands), `(& x x)` and `(| x x)` become x, known
  branches their arm, a while whose test is false at every look nothing.
- `licm*.cpp`: outer loops first; a pure expression whose inputs the loop
  (and its calls' summaries) never write moves into a new local's let before
  it: from a while's condition always, from the body only what runs whenever
  the body does (not arms, not after an early exit, not in an inner loop that
  may not run), behind a guard (`(if (< start end) ...)` or the condition
  again) when the loop may run zero times. Never loads, :address or aux
  reads, bools, widened bytes, `(+ p i)` under a load or store, or work worth
  less than ~12 cycles; nothing out of a loop of one pass, no quarter-square
  multiply out of every loop from a loop of two.
- Dead locals keep their frame bytes: the interpreter runs the program as
  written on the compiled layout (`InitInterp(..., compiled.layout)`, also
  `tisple interp`), so every source local needs bytes of its own; giving them
  none would need a separate interpreter layout for frames.
- `--no-pass consts|licm`. Results: dogfood.md 40.

Passes 6 and 7 (2026-10-29); each round now runs dead, loops, eval, consts, licm,
cse:

- `cse*.cpp`: a walk of each function in the order it runs keeps the values
  worked out so far that still hold (`cse_values.cpp`: integer and enum
  arithmetic and calls to pure functions that do not load, over variables with
  no :address, aux or :reserve). Not array reads: the backend reaches an
  element again cheaply and updates `(set (at a i) (+ (at a i) x))` in place.
  A value stops holding at a set or let of what it reads, a call whose summary
  writes it (or does I/O or jsr), a store through a pointer when it reads an
  address-taken variable, io_read, io_write, jsr and unknown nodes; a loop
  stops, from its start, what it changes anywhere in it. A value first worked
  out in a do's item (not in an arm, the right side of an and/or, or a while's
  condition) goes into a let before that item when nothing earlier in the item
  changed what it reads, and is read at every later working out in the rest of
  the do, arms and loops included; a void arm that is not a do is wrapped in
  one. Only when k readings save k times licm_cost's estimate less a store and
  k + 1 reads, by 12 cycles or more.
- `(/ a b)` and `(% a b)` apart: both lets go side by side and the backend
  divides once; not when only straight code (no branch, loop, call or routine)
  lies between, which the backend shares already. Its let forwarding
  (`index_alias.cpp`) no longer moves either half of such a pair to its reader.
- Facts from conditions (`consts_facts.cpp`, in the constants walk): where a
  pure condition `(= x k)` held (or `(!= x k)` did not; through `not`, a taken
  `and`, an untaken `or`), x holds k, a literal or a variable known there; in a
  match clause the variable matched on holds the clause's value; after a while
  that ends by its condition, the condition was false. Ranges from `<` and `>=`
  not done: in 300 race programs one literal compare of a variable sits inside
  another of the same variable, and the compares are mostly loop heads whose
  counters the backend's ranges already know.
- `eval.cpp`: a call with constant arguments, to a function whose summary is
  pure (no writes, I/O, jsr or unknown nodes, every loop provably ends) and that
  reads no global and nothing through a pointer, runs in the reference
  interpreter on a LayoutProgram layout of the program as it is now; the call
  becomes its value. Budget: 20,000 interpreter steps a call, 400,000 a
  compile; a call over its budget stays a call and is not tried again.
- `--no-pass cse|eval`. Results: dogfood.md 42.

Slice 4 (2026-10-29), what llvm-mos deleted in the worst race losses: loop collapse, value
ranges, unread arguments. Each round now runs dead, loops, args, collapse, eval, consts,
ranges, licm, cse:

- `collapse*.cpp`: a for, or a counting while ((let c v) just before, (while (< c k) ...
  (set c (+ c s)) ...)), whose passes all do the same runs its body once. A pass does what
  the one before did when no cell it writes (a variable's bytes, or one element or field at
  a literal index) is one it reads, the body's own variables aside: its lets and fors, and a
  scalar a pass sets before anything in the pass reads it. Calls read and write
  what their summaries say. Loads and stores reach what provenance says
  (`collapse_memory.cpp`: addr_of, a local one let sets, a parameter nothing writes followed
  to what the calls pass, a call's return); a load or store of memory no variable uses keeps
  the loop, as do I/O, jsr, wait_cycles, :address, aux and :reserve variables, unknown
  nodes, a break, continue or return that ends a pass, and a body over 3000 nodes.
  - A for whose range the shapes do not decide runs behind (if (< a b) ...); a and b are
    still worked out once.
  - Accumulators, (set x (op x k)) with x read nowhere else, add up: (+ x (* k n)), (^ x k)
    for odd n, (| x k) and (& x k) once; + - ^ only when n is known.
  - A body that reads its counter collapses to its last pass, the counter's last value put
    in, when every write outside its own variables lands in the same place on every pass:
    not under a branch on the counter or what it makes, not in a loop inside whose range
    differs or that may end early, no write by a call whose arguments differ, and the
    accumulators add what does not differ.
  - Inner loops first, so a nest collapses level by level.
- `ranges*.cpp` (pass 8, in part): the values an expression may hold, from its shape (masks,
  remainders and quotients by constants, shifts, sums and products that cannot wrap,
  widenings, const tables) and the counters of the loops around it (a for's variable over
  its range, a counting while's counter in [v, k - 1 + s]), decide compares, cut match
  clauses no value reaches (a match left with its wildcard becomes it), and decide
  (and x false) and (or x true). Only quiet operands: pure, no :address, aux or loads.
  (~ (~ x)) and (not (not x)) become x, so (set g (~ (~ g))) is a dead store.
  Ranges are not carried through lets or sets; the backend's byte ranges (ranges.cpp there)
  are a separate thing.
- `args.cpp`: an argument for a parameter its function never reads becomes 0 when it is
  pure and quiet. Variable reads too: an inlined call then stores the 0 where the backend
  would have bound the variable to the parameter, a few cycles, but a loop around a call
  that reads what the loop writes does not collapse. Replacing them measured better than
  leaving them for inlined calls (1.444 to 1.442).
- licm: no FasterInLoops rule (the backend picks the multiply routines once per program from
  run counts); a constant multiply costs what gen_mulconst.cpp emits: binary or signed
  digits, byte products always inline, 16-bit ones inline up to four digits.
- `--no-pass args|collapse|ranges`. Results: dogfood.md 46.

Slice 5 (2026-10-29), from the worst race losses after slice 4. Each round now starts by
working out what every variable may hold (ranges_vars.cpp) and runs dead, loops, args,
returns, collapse, eval, consts, ranges, licm, cse; once a round changes nothing, collapse's
walk unrolls and unswitches, and the rounds go on:

- `ranges_vars.cpp` (`vars`): what each scalar with no address taken ever holds, from every
  write: a global's initial value and its sets, a local's let and sets, a parameter what
  every call passes (a function nothing calls: anything). A local stepped once a pass,
  (let v a) in a do and a later loop of at most n passes whose body (not a loop inside)
  holds its only other writes (set v (+ v s)), lies in a + n * s, in the type or wrapped
  once as a whole; a step's range in the other signedness is taken when narrower (a u16 of
  65535 or 1 steps by -1..1). Three walks from the types' ranges; each holds. Only functions
  main reaches; off with jsr or nodes not known; with no main only locals (anything may be
  called from outside, memory written between calls). A range of one value is a literal: 741's
  `a24`, which every call passes 65535.
- `ranges_elements.cpp` (`elements`): the packed bytes each set rooted at an array or struct
  global may write, from its index ranges; a read of bytes none reaches is the initial value
  ((at pt 12) is 0 in most race programs). Not for globals a pointer store may reach
  (collapse_memory.cpp's provenance), hardware ones, or when a set's index may fall outside
  its array (a neighbour's bytes would change).
- `ranges.cpp`: after (set x v) or (let x v) among a do's items, x has v's range in the items
  after it until something may write it (sets, calls by their summaries, io_write and jsr for
  globals, a loop from its start for what it writes anywhere). (set g (% g 9)) (set g (/ g
  9)) leaves g 0. Integer and enum values whose range is one value become literals; enum
  compares and matches are decided. Shapes: shifts left, products, and sums that wrap as a
  whole keep a range; signed quotients and remainders; (| x k) is at most x + k; an if's value
  either arm's. ValueRange is memoised while what is around a node stays.
- `collapse_continue.cpp`: (if c (continue)) items of a loop body make the rest of the pass
  their else, so the body ends each pass at its end (seed 76).
- `collapse_count.cpp`: accumulators of a for whose passes are not known add (b - a) times,
  worked out behind its guard from copies of the bounds (pure, no hardware, nothing the loop
  writes). A for of no passes goes whatever its body does.
- `unroll.cpp` (`unroll`): once the passes settle, a loop of at most 8 passes whose copies
  come to at most 96 nodes, with no calls (copies stop the backend inlining) or hardware
  (timing), and no early exit, becomes a copy of its body per pass, the for's counter a
  literal in each; the rounds after fold the copies ((| i v) over -3..3 is -1). Not waiting
  for the passes to settle unrolled bodies they would have shortened enough to collapse.
- `unswitch.cpp` (`unswitch`): once the passes settle, a for whose body's first if or match
  item tests what the loop never changes (pure, quiet, nothing the loop writes; cheap when the
  loop may make no pass) becomes that branch with a copy of the loop per arm, when at least
  one copy then collapses (without that, the copies cost bytes and sometimes cycles: an
  address the loop no longer changes is still worked out every pass).
- `args.cpp` (`returns`): a function something calls whose call values nothing uses returns
  literal 0 from its pure return values (585's (* g6 59719) per call). Only with a main.
- consts: (+ x (~ x)), (| x (~ x)), (^ x (~ x)) are all ones, (& x (~ x)) is 0; a bool
  variable tested is false in the else arm (after (if flag (return))); not true in the then
  arm, which a byte loaded as bool need not hold as 1. dead: statements after a return, break
  or continue go, so they leave the function's summary. The counting-while rules
  (KnownTrips' let before) look past lets of other variables.
- Closed world: with a main, only main is called from outside; tests that call other
  functions by address must call them only with what main's calls would pass, and must not
  poke globals between calls.
- `--no-pass vars|elements|unroll|unswitch|returns`. Results: dogfood.md 48.

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
