# Tisple ][ design

Started: 2026-10-26. Grew out of `~/Coding/Ideas/apple2-language-compiler.md`.

Tisple ][ (typed Lisp for the Apple II) is a small, typed, Lisp-shaped systems
language with its own optimizing 6502 backend. Written "Tisple ][" in prose and
banners; `tisple` for the command, repo and folders; `.tsp` for source files.

Everything below marked *proposed* is a starting point, not settled.

## Why a new language can beat C on the 6502

LLVM-MOS and Oscar64 are very good, but C's semantics fight the chip. Tisple
wins by not having those semantics in the first place:

1. **No integer promotion.** C promotes to 16-bit `int` and the compiler then
   fights to narrow back. In Tisple `u8` is the common case and widening is
   always explicit.
2. **Struct arrays have no promised layout.** `sprites[i].x` with a 3-byte
   struct costs a multiply in C. Tisple stores arrays of structs as one array
   per field by default, so it becomes `LDA sprite_x,X`. Arrays of 256 or fewer
   elements index with `u8`, so every index fits in X or Y.
3. **Recursion is opt-in.** Non-recursive functions get static frames, and the
   call graph lets frames that never live at the same time share bytes. Calls
   become nearly free and zero page goes much further.
4. **Placement facts reach instruction selection.** If the linker guarantees
   `boundary 256`, `LDA table,X` never crosses a page, so its cost is exact. In
   C that knowledge lives in a linker script the optimizer never sees.

## Cycle costs known before running

The 6502 has no cache, no branch prediction and no out-of-order execution.
Every instruction's cycle count is fixed and documented (the opcode fetch
overlap is already in the published numbers). On a stock II+/IIe video shares
memory without stalling the CPU. So the compiler can know what code costs:

- straight-line code: exact.
- branches: +1 taken, +2 taken across a page. An `if` has a cost per arm; the
  compiler reports min/max and can pad the short arm to balance them.
- indexed reads: +1 on page crossing, which placement can rule out.
- loops: exact with a known trip count, otherwise a formula ("12 + 9 per
  element").
- ROM, ProDOS and assembly calls: unknown unless annotated. These are the
  honest holes.

Plan: `tisple build --cycles` prints a cost per function. Later, *proposed*: a
`cycles 23` assertion on a block that fails the build if the code drifts. Aimed
at speaker sound (all cycle-counted `$C030` toggling) and video tricks. No
existing 6502 compiler does this; it is the feature people would come for.

Accelerators (Zip Chip, IIc+ fast mode) break real-time timing, and every 65th
Apple II cycle is stretched in wall time (still one cycle). The target machine
is chosen explicitly.

## Syntax

Plain parentheses carry the structure. Whitespace means nothing to the reader:
a whole program mashed onto one line is the same program. `tisple fmt` owns the
layout, the way clang-format does in splonks-cpp: 4 spaces, 100 columns, one
canonical result.

```scheme
(fn fill ((dst (ptr u8)) (count u16) (value u8)) void
    (for i u16 0 count
        (store (+ dst i) value)))

(fn read_key () u8
    (let key u8 (io_read u8 0xC000))
    (& key 0x7F))
```

Settled:

- Small vocabulary: `fn`, `let`, `const`, `struct`, ordinary control flow.
- Rust naming: `PascalCase` types, `SCREAMING_SNAKE_CASE` constants, `snake_case`
  everything else including built-ins. No kebab-case, no `!` suffixes.
- Round parentheses only; no square brackets.
- `(fn name (params) type body ...)`: the parameter list is always written,
  `()` when empty, and the return type is always third, `void` when nothing is
  returned.
- `let` for locals and top-level bindings. No `define`, no `global`.
- Every declaration states its type: `(let key u8 ...)`, `(const MAX u8 32)`,
  `(for i u8 0 n ...)`. Nothing is inferred but the type of a literal.
- A type never heads a form: `(as u16 y)` converts, `(make Sprite ...)` builds.
  The four shapes (declare, build, access, control) are in `docs/language.md`.
- The formatter is designed with the grammar, not after it.

Reader: a symbol is any run of characters other than whitespace, parentheses,
`;`, `"` and the quote marks (so `+`, `<=`, `&`, `|`, `.`, `hgr.plot` and
`gfx/hgr` are all symbols); `#` may only end a name (`tmp#`). `:name` is a
keyword atom. Integers are decimal, `0x` hex or `0b` binary, optionally
negative. Strings are one line, with escapes
`\n \t \r \0 \\ \" \xHH`. Quote marks read as lists (`'x` is `(quote x)`,
`` `x `` quasiquote, `,x` unquote, `,@x` unquote_splicing), must touch their
form, and print back as marks. Comments run from `;` to the end of the line;
they live beside the tree, not in it.

Formatter rules (`tisple fmt`, implemented in `src/reader/format.cpp`):

- Every line is indented a multiple of 4, measured from the line where the form
  starts, not from its parenthesis.
- Body forms always put their body on new lines at +4, like braces on every
  `if`: `fn` (name, parameter list and return type stay on the head line),
  `macro` (name and parameter list stay), `if` (the condition stays; both arms
  are body), `while`, `match` (the value stays; each clause a line), `for` (`i`,
  the type, start and end stay), `do`. With no body they stay on one line:
  `(while (not (key_ready)))`, `(do)`, `(fn f () void)`.
- A `match` clause is a body too, with the pattern as its head: one line when
  it is a single form that fits, `(Action.left (step 255))`, or nothing,
  `(_)`; otherwise the pattern stays and each form goes on its own line at +4.
- Everything else stays on one line when it fits in 100 columns, closing
  parentheses included. Otherwise the head and the first argument stay (the
  first argument goes down too if it does not fit), each remaining list
  argument gets its own line at +4, and runs of atoms fill lines.
- Declarations (`let`, `const`) keep the type on the head line with the name,
  even a list type: `(const ROW_START (array u16 24)`, then the value at +4.
- Data lists: in a list that takes several lines, a run of atoms keeps the
  author's line breaks, because a table's layout means something (8 row
  addresses a line in `lib/lores.tsp`). An atom that starts a line in
  the source starts one in the output; the others fill the line to 100
  columns. A list that fits on one line still goes on one line, and with no
  breaks to keep (a mashed line) the values pack.
- A body form as the last argument hangs off the head line when the head line
  fits: `(let m u8 (if c` then arms at +4.
- Parameter lists (`fn`, `macro`) and field and member lists (`struct`, `enum`)
  stay on the head line: one line if they fit, else the first item stays right
  after the `(` and every other item goes on its own line at +8, so it never
  looks like body; the return type follows the closing `)`:
  `(fn draw ((dst (ptr u8))`, `        (count u8)) void`.
- A `:keyword` and its value stay together on one line.
- A header item that cannot stay on the head line (a comment forced it down)
  goes to +8, so it never looks like body.
- Comments: an own-line comment stays above the form it precedes, at that
  form's indent; a trailing comment stays at the end of its line and forces
  a break after it; closing parentheses move in front of it, `(a)) ; done`.
  Consecutive trailing comments line up when that stays inside 100 columns. A
  comment right after `(` moves to the end of that line. A comment before `)`
  stays inside, and the `)` goes on its own line.
- Blank lines: one blank line is kept where the source had one or more, never
  directly after a head line. Top-level forms that take several lines always
  get a blank line on both sides; one-line forms (`const` groups) may stay
  together.
- Output ends with one newline. Formatting is idempotent, keeps the tree, and a
  program mashed onto one line formats to the same text as the hand-laid-out
  one minus comments, blank lines and the line breaks in data lists;
  `tisple_tests` checks all three on every file in `examples/`, `programs/`
  and `lib/` and on random programs.

An indentation-only syntax (wisp, SRFI 119; Racket's Rhombus) could be added
later as a second reader producing the same tree. Not planned.

## Core language

- `bool`, `u8`, `i8`, `u16`, `i16`. Arithmetic wraps. Binary ops need both
  sides the same type, literals take the type their context needs, widening
  and narrowing are explicit (`(as u16 x)`).
- Functions, locals, `if`, loops, early `return`.
- Structs, fixed arrays, constants, byte strings.
- Pointers, address-of, load/store, indexing, field access.
- Hardware reads/writes the optimizer never drops or reorders (`io_read`,
  `io_write`); reading `$C000` and touching `$C030` both have effects.
- Inline assembly and ROM-call interop with a declared register/flag ABI.

No `malloc` in the language. Fixed arrays and fixed-capacity collections cover
most programs; arenas and pools are libraries if ever needed. Do not add range
types, effect contracts or aliasing contracts just because they might help the
optimizer. Add features when real programs need them.

### Memory and placement

Storage owns bytes; a view gives typed access to existing bytes; placement
chooses where something lives. Placement constraints are checked at final link
and the build fails if one cannot hold:

| Form | Meaning |
| --- | --- |
| `address 0x6000` | Start at this exact address. |
| `in tables` | Allocate inside named storage or a region. |
| `align 256` | Start on an address divisible by 256. |
| `boundary 256` | Keep the whole object inside one 256-byte block. |

`zero_page` is a target-defined region (minus monitor/ProDOS reservations), so
`in zero_page` lets the allocator choose. Representation (field-split vs packed
structs, stride) stays separate from placement. A struct array is packed only
when the program needs its bytes as one block (a view, address-of an element,
assembly interop).

### Macros

Common Lisp-style quasiquote templates with Clojure-style `name#` unique
names. Details in `docs/language.md`.

## Backend

- A/X/Y pressure with each instruction's register restrictions.
- Whole-program zero-page allocation and static frame overlap.
- Carry and other flags as live values.
- Instruction selection over the addressing modes.
- Page-crossing costs, branch layout, alignment.
- Constant propagation, specialization, inlining, dead code.
- Short-sequence search with equivalence checking (the emulator is the oracle).
- Explicit size vs speed objective; they often conflict.

Recursion, function pointers, assembly, ROM calls and interrupts break static
frame sharing and register assumptions. Each gets a defined ABI; never assume
calls cannot overlap.

## Target

*Proposed* first target: NMOS 6502 (runs on every Apple II), Apple IIe memory
map, raw binary loaded at `$0803` or `$6000` by `BRUN`. 65C02 (enhanced IIe,
IIc) as a later flag; its instructions and timings differ and must not be
mixed up. ProDOS SYS files later.

## Implementation

C++20 in the splonks-cpp / Adventures with Chickens style: C+- code, small
files by domain, splonks `.clang-format`, see `AGENTS.md`. *Proposed* layout:

```text
src/main.cpp        tisple command: build, sim, fmt, show, cycles
src/reader/         tokens, tree, formatter, highlighting
src/check/          names, types, inference
src/interp/         reference interpreter (the correctness oracle)
src/ir/             lowered program the backend works on
src/backend/        isel, register/zero-page allocation, frames, cycle costs
src/link/           placement, constraint checks, binary output
src/emu/            cycle-exact 6502 core and test harness
src/machine/        built-in Apple IIe: soft switches, video, speaker, window
tests/              unit, round-trip, fuzz
races/              drag racing against other compilers
```

## Testing

- 6502 core: floooh's chips `m6502.h`, vendored in `third_party/chips/`, behind
  our wrapper in `src/emu/` (load, set registers, run to a stop point under a
  cycle limit, return state and cycles; I/O hook for `$C000`-`$CFFF`). Passes
  all 1.51M SingleStepTests cases for the 151 documented opcodes, bus cycles
  included (`scripts/fetch_singlestep.sh` gets the data). About 166M simulated
  cycles/s in release.
- Reference interpreter runs the checked tree. Compiled output must match it on
  every test and on randomly generated programs.
- Exhaustive tests for small operations (every `u8` pair for add/sub/compare).
- Full-machine tests later through a headless emulator (izapple2 or MAME):
  startup, text, hi-res, speaker, ProDOS.

## Drag racing

Contestants: cc65, LLVM-MOS, Oscar64 (if it can target the Apple II), Tisple,
and handwritten assembly as the time to beat. Events: 16-bit multiply, memcpy,
sieve, string search, hi-res sprite blit, line drawing, speaker music loop.
Scored on cycles (exact, from the emulator) and bytes. Publish versions, flags
and inputs. Showpiece: the same hi-res routine from each compiler running side
by side, watching who finishes first.

## Prior art

- [LLVM-MOS](https://github.com/llvm-mos/llvm-mos) and its
  [SDK](https://github.com/llvm-mos/llvm-mos-sdk) (has an Apple IIe target).
- [Oscar64](https://github.com/drmortalwombat/oscar64): optimizing C for
  Commodore machines; tops the [c-bench-64](https://thred.github.io/c-bench-64/)
  comparisons.
- [cc65](https://cc65.github.io/doc/apple2.html): the long-standing baseline.
- Millfork: a middle-level language designed for the 6502. Read first.
- Prog8 (C64/X16), KickC, Cowgol: other 6502-native languages.
- PLASMA: native Apple II language on a bytecode VM.
- GOAL/GOOL (Naughty Dog): typed Lisps for game consoles.
- [SingleStepTests 65x02](https://github.com/SingleStepTests/65x02),
  [izapple2](https://github.com/ivanizag/izapple2),
  [MAME](https://docs.mamedev.org/commandline/commandline-all.html),
  [W65C02S datasheet](https://www.westerndesigncenter.com/wdc/documentation/w65c02s.pdf).

## Milestones

First priority is a compiler that works, a small simulator to run what it
makes, a fast build, and a colored viewer for reading `.tsp` files. Cycle
reports come after the compiler works.

1. Build and test runner (done: CMake + Ninja + ccache + lld, `tisple_tests`).
2. 6502 simulator passing SingleStepTests with exact cycle counts; `tisple sim` (done).
3. Reader, formatter (`tisple fmt`), colored viewer (`tisple show`, editor
   syntax for VS Code and Neovim) (done).
4. Checker and reference interpreter for the core subset.
5. Plain backend: compiled programs run in the simulator and match the
   interpreter under fuzzing.
6. Field-split struct arrays, static frames, zero-page allocation.
7. Computing macros (compile-time evaluation): tables, sprites, generics, unrolling.
8. `--cycles` reports.
9. First race against cc65 and LLVM-MOS on three kernels.
10. Full Apple II: a hi-res demo booting in a headless emulator.

## Open decisions

- Literal typing details, pointer arithmetic, signed compares.
- View syntax.
- How backing storage for allocators is declared.
- Calling convention for recursion, function pointers and interrupts.
- IR shape.
