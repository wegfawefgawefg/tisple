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
3. **No recursion.** The checker rejects call-graph cycles, so every function
   gets a static frame, and the call graph lets frames that never live at the
   same time share bytes. Calls become nearly free and zero page goes much
   further. (Recursion, if it ever comes, would be opt-in; docs/language.md.)
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
negative. A char literal, `'A'`, is an integer token (its text kept as written,
its value 65): `'`, one character or one of the strings' escapes (and `\'`),
`'`, then the end of the token (a space, `)`, `;` or the end). Strings are one
line, with escapes `\n \t \r \0 \\ \" \xHH`. Quote marks read as lists (`'x`
is `(quote x)`, `` `x `` quasiquote, `,x` unquote, `,@x` unquote_splicing), must
touch their form, and print back as marks, except a quote of a list that starts
with a char literal, which prints as `(quote (' ' x))`: `'(' ' x)` would read
back as the char `'('`. Comments run from `;` to the end of the line; they live
beside the tree, not in it.

Formatter rules (`tisple fmt`, implemented in `src/reader/format.cpp`):

- Every line is indented a multiple of 4, measured from the line where the form
  starts, not from its parenthesis.
- Body forms always put their body on new lines at +4, like braces on every
  `if`: `fn` (name, parameter list and return type stay on the head line),
  `macro` (name and parameter list stay), `if` (the condition stays; both arms
  are body), `while`, `match` (the value stays; each clause a line), `for` (`i`,
  the type, start and end stay), `do`, `with_aux` (reads and writes stay). With no
  body they stay on one line:
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
- ROM and firmware calls, `(jsr addr (a v) (x v) (y v))`; no inline assembler
  (docs/language.md, Machine code).

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
- Whole-program zero-page allocation and static frame overlap. Locals the
  mid-level passes make (inlined copies') get bytes by liveness over the
  finished code, a copy's in the frame of the function it came from
  (src/backend/frame_share.hpp).
- Carry and other flags as live values.
- Instruction selection over the addressing modes.
- Page-crossing costs, branch layout, alignment.
- Constant propagation, specialization, inlining, dead code.
- Short-sequence search with equivalence checking (the emulator is the oracle).
- Explicit size vs speed objective; they often conflict.

Recursion, function pointers, assembly, ROM calls and interrupts break static
frame sharing and register assumptions. Each gets a defined ABI; never assume
calls cannot overlap.

### Calls through function pointers (2026-10-30)

Made into direct calls once the mid-level passes are done
(src/backend/dispatch.cpp), so frames, effects, ranges, pointer targets and
inlining see ordinary calls:

- Each `(fn ...)` type t has a dispatcher: a function whose parameters are the
  pointer and then t's parameters. `(p a b)` is a call of it: the pointer and
  the arguments go into its frame like any call's, then `JSR`. Its code is
  `LDA p; STA j+1; LDA p+1; STA j+2; j: JMP $0000` (the program runs from
  RAM, the card's included).
- Each function f whose address is taken has an entry, `f.indirect`, with no
  parameters of its own: its body is f called with the dispatcher's
  parameters, so f is usually inlined into it and reads them where they are. A
  function pointer's value is the entry's address; the interpreter, run on the
  compiled layout, uses the same values (`Layout::functions`).
- The entry returns straight to the dispatcher's caller: results in A, or A
  (low) and X (high), as for any call.
- The call graph is caller, dispatcher, entries of t's functions, functions.
  Frames stack in that order, so the dispatcher's frame, which holds the
  arguments, is live while an entry runs and nothing that runs then overlaps
  it. For the analyses the dispatcher's body is a match that calls one of the
  entries; its code is only the jump.

### Interrupt handlers (2026-10-30)

How the machines route an IRQ (checked in their roms with our NMOS core, which
runs all three IRQ paths; tests/test_backend_irq.cpp):

- II+ and unenhanced IIe: $FFFE holds $FA40, which does `STA $45`, looks at
  the B flag on the stack (a BRK goes to the monitor) and does `JMP ($03FE)`.
  The handler finds the interrupted A in $45 (A itself holds the status shifted
  left), X and Y as they were, and must give A back from $45 before its `RTI`. With RAMRD on, `$3FE` is read from
  aux memory.
- Enhanced IIe: $FFFE holds $C3FA, which saves A, X, Y and the memory switches
  on the stack, puts main memory and the rom in (the card's ram too), pushes a
  return into itself and does `JMP ($03FE)`; after the handler's `RTI` it puts
  everything back. $45 is not touched on this path (only a BRK writes $44).
- Language card ram read-enabled: the cpu reads $FFFA-$FFFF from the card,
  whatever the rom says. Our programs have the card in for all of main when they
  use it, so with a handler the link keeps $FFFA-$FFFF out of the card's data
  and the startup writes them: NMI $03FB and RESET $FA62 as the roms have them
  (reset puts the rom back in anyway), IRQ straight to the entry.

What the backend makes (src/backend/irq.cpp):

- `irq.direct: STA $45` (the card's $FFFE), then `irq.entry` ($3FE): `CLD`,
  X and Y pushed (each only if the handler's kept code, or a runtime routine it
  calls, may change it: any LDX, TAX, INX and the like, or a JSR or JMP to an
  address, which could be anything), then each zero-page scratch byte the
  handler's kept code uses, or every scratch byte the runtime routines it calls
  use; `JSR handler`; the same pulled back; `LDA $45; RTI`. 45 cycles and 19
  bytes with X and Y (34 and 15 with X alone) besides the scratch bytes (13
  cycles each), the rom's path not counted.
- $45 is never given to a variable in a program with a handler (an :address
  let there is an error).
- The handler and everything it calls get frames above every other function's:
  the handler counts every other function as a caller. Nothing else shares
  their bytes, so they are not saved; main's frames, its temporaries, page and
  loop pointers are never touched.
- Startup: `PHP; SEI`, zero page saved and set, the old $3FE kept and the entry
  put there (and the card's vectors), `CLI`, `JSR main`; then `SEI`, $3FE as it
  was, zero page back, `PLP; RTS`.
- What the handler shares is `Var::irq_shared`: the symbol is `shared`, which
  keeps the peephole and code generation from taking a register to hold it,
  the dead-store pass from it, and every "may this code write it" from saying
  no.
- The handler runs over and over while main does: run counts (and so the size
  budget and the choice of multiply routine) count it as often as a while
  loop's body, 8 times a run of main, and its callees by their calls from it.

### Static data (2026-10-30)

docs/language.md, "Static data", is the language; this is how it is made.

- One mechanism for every address in initial values: `AddrSlot` (was `FnSlot`,
  which only const tables of functions had), beside a var's packed bytes: at a
  byte offset, a function's value or byte `at` of a var. The checker folds a
  value into bytes and slots (src/check/data.cpp, `FoldValue`, `FoldAddress`);
  the layout fills the slots in (src/link/image.cpp, `SlotValue`), for the
  binary and the interpreter alike. Only pointer and function pointer values
  hold one, so every integer byte any pass reads from `Var::init` is the real
  one.
- A struct or array const is a `VarKind::Const` var (was `ConstArray`), made
  before its value is folded so the value may name its address; reading its own
  bytes while they are folded is an error. A top-level let is resolved on its
  first use (`EnsureGlobal`), so a const may take its address before the lets
  are gone through. A pointer const keeps its checked value and each use gets a
  copy (`CloneExpr`), as a string const always did.
- `ConstValue` reads const data at constant places (`ConstPlace`) and loads at
  constant addresses into it (`ConstAddress`), unless a slot is there: the
  backend's folds and the mid-level's see the same values, at -O0 too.
- Pointer targets (backend/targets.cpp): a slot's var is in the heap's targets
  (reads of elements and fields), and in a global pointer's own.
- What the binary holds (backend/link_data.cpp): a const kept code names, with
  an `:address`, or that a slot of a held var points into. The program as
  written runs in the interpreter on the compiled layout, so a const whose
  address it may take (an `addr_of` into it in code reached from main or the
  handler, or a slot of what that code reads, `FindAddressedConsts`) and the
  binary does not need gets an address inside the code: the interpreter never
  reads or writes there, and the 6502 has code there. Every other const the
  binary leaves out the interpreter reads from its bytes (`Interp::left_out`):
  const data never changes, and a place in it on the layout has address 0 and
  packed bytes, so the cursor's address is the byte offset.

### I/O bases (2026-10-30)

A card found at run time is reached at its slot's page plus a constant:
`(io_write u8 (+ page 1) v)` with `page` a global init sets. An access at a
computed address stores the address into its own instruction first (20 cycles
for a 4-cycle `STA`); a register write of the Mockingboard is six of them. So
the backend makes such globals I/O bases (src/backend/io_bases.cpp):

- A base is a u16 global with no `addr_of`, no `:address`, never set in the
  interrupt handler's code, that some `io_read` or `io_write` uses as its
  address, alone or plus or minus a constant.
- Each such access is one `LDA` or `STA` absolute whose operand is the symbol
  `<base>.io` plus the constant: until the link a symbol at $C000 that is
  `shared`, so no pass tracks, merges or drops it, as with a constant I/O
  address; at the link the base's initial value.
- Each `(set base v)` becomes `(do (set base v) (base.patch))`. The patch
  function's code is made once every function's code is final: for each
  constant K, `LDA base; CLC; ADC #<K`, `STA` into the low operand byte of each
  access with that K, `LDA base+1; ADC #>K`, the same for the high bytes; with
  `PHP; SEI` and `PLP` around it when there is a handler, so the handler never
  runs on half an operand. A label goes before each access it patches. The
  startup calls each patch once before main (zero page is set again on each
  run; the code keeps what the last run patched).
- An `io_write` whose value is worked out after its address must not set the
  base on the way (a set of it, a call that may set it, machine code): then the
  access stays computed, as the interpreter reads the address first.
- Tearing: a set from main with interrupts on can be stopped between storing
  the base and the patch; the handler then sees the old address, as it would
  have between the two bytes of the store before.

The Mockingboard's register write went from 20 cycles an access (or a 14-arm
match on the slot to reach fixed ones) to 4.

### Parameters handed on (2026-10-30)

A small function inlined by the backend, whose only call is its last act, has
its parameters live in that call's parameter bytes when it passes them on
unchanged (src/backend/gen_forward.cpp): `(fn reg ((i u8) (v u8)) void (if (!=
(at sent i) v) (send i v)))` inlined puts its arguments straight into
`send.i` and `send.v`, reads them there, and the call copies nothing. Nothing
else runs a call before it, so nothing whose frame may share those bytes runs
in between; nothing reads the parameters after it, so what `send` does to its
own does not matter. Not for a parameter the copy binds to its argument, one
with an alias or its address taken, or a callee the backend copies too.

### Room (2026-10-30)

The size budget (src/backend/size_budget.hpp) weighs every copy of a call and every unrolled
loop by the cycles it saves for its bytes. It did not know how much memory there is, so a
program that filled main memory and the card (Catwiz) needed 128 `--no-inline` names to fit.
Now the link decides (src/backend/squeeze.cpp):

- A program that fits is built as before (squeeze 0).
- One that does not (code or data, after the language card too) is built again with a
  tighter budget, a level at a time up to 10, the first that fits kept. Each level halves
  the item, function and program budgets and doubles the cycles a byte that make an item pay
  for itself (32 at 0, none at 10: then nothing that adds bytes is copied). The
  quarter-square multiplies count their 1 KB of tables and routines as bytes the first
  multiply pulls in: squeezed, they are used only when the multiplies save the level's cycles
  for each of those bytes (80 cycles a multiply counted). From level 4 the mid-level passes
  neither unroll nor unswitch.
- The build says what it traded, a warning: the level, the functions' and runtime's bytes
  against the usual build's, the functions now called that the usual build copied at every
  call, the loops left rolled, the multiply routines.
- `--no-inline` names must be functions; the error says what the names look like
  (`draw/text.start`), or the one meant.
- Before a tighter budget, the link tries the card's bank 1 for whole cold functions behind
  bank-switching stubs, 20 cycles a call across (docs/overlays.md, Built). A program that
  fits that way is also built with the tighter budgets without it, and the one with fewer
  cycles a run, about, is kept; the warning says what was traded. Catwiz: squeeze 3 became
  squeeze 0 with 3.3 KB in bank 1, its loops 3-4% faster.

### Aux memory code (2026-10-31)

docs/language.md, "Aux memory code", is the language: `(with_aux reads writes body ...)`.
Before it, five libraries carried 6502 code as byte arrays for one reason: compiled code could
not run with RAMRD or RAMWRT on. How it is made:

- The checker makes the form `(do on body ... off)`: two `AuxSwitch` nodes, on with the two
  bools, off with the body's last form as its value (worked out before the switches go off).
  A return in the body gets an off around its value; a return without one, and a break or
  continue to a loop outside, an off before it (check/with_aux.cpp). No pass needs to know
  regions: every pass treats the switches as I/O.
- check/aux_mode.cpp follows the switches through each function's tree in evaluation order
  (joined where paths meet, to a fixed point around loops, a return or break ending a path)
  and through calls: a call made where a switch may be on runs its callee with it on, an
  aux-mode function. The checker's rules (check/aux_rules.cpp), the pointers pass, the backend
  and the interpreter all read it; it is worked out again on the optimised program, so it
  holds after inlining, unrolling and unswitching.
- Frames: every variable an owner (a function with a region) or an aux-mode function names,
  and their temporaries, are forced into zero page (frames.cpp), and their symbols marked
  zero page before code generation. An owner is never inlined (FindInlines, the mid-level
  inliner), so its region stays where its frame is.
- Memory knowledge: in owners and aux-mode functions the peephole tracks only zero-page
  symbols and no indexed byte, and the code generator forgets the registers' memory bytes
  outside zero page at each switch. Loads that may run with RAMRD on have no range (ranges'
  aux_reads), and the pointers pass leaves those loads, and stores with RAMWRT on, as they are.
- Mirroring: functions whose code may run with RAMRD on, the runtime when they call it, and
  the const data such code reads by name go into aux memory segments at the same addresses
  (backend/aux_link.cpp): the code at the start of the memory below the origin when there is
  one (lets `:in aux` fill aux memory from `$0800`, so that cuts their room the least), else
  after the other code; lets `:in aux` are laid out around both (LayoutOptions::aux_reserved;
  the layout runs a second time when const data needs room there, main memory's coming out
  the same). Never in the card's bank 1. The interpreter's aux memory gets the same const data
  (Layout::mirrored).
- Code: on is `STA $C003`/`$C005` for a constant, `LDX b` / `STA $C002,X` (and `STA
  $C004,X` when writes is the same bool) for a variable; off is `STA $C002`/`$C004` for what
  the walk says may be on there.
- Ported to it: lib/auxmem's copies, lib/dhgr's boxes, plot and text_row, lib/dhgr_pixels.
  Each runs as fast as the byte arrays did or faster (docs/dhgr.md); three code generator
  and link changes came out of it (a signed compare of a pinned register with 0 is its N
  flag, so a count-down loop ends in DEY / BPL; a pointer or word moved by a small constant in
  place is ADC / BCC / INC; up to 32 bytes before a function keep its innermost short loops
  inside a page, when the program is not squeezed: link_code.cpp, PadLoops). Left as 6502
  code: lib/unpack's decoder (it flips RAMRD between the packed bytes and the output in the
  middle of a run, which one region cannot, and its aux card mode needs ALTZP), lib/auxcard and
  dhgr.runs (ALTZP for the card; one decoder serves its three places), auxmem.guard (interrupt
  glue in page 3).

### Const data in aux memory (2026-10-31)

docs/language.md, "Const data in aux memory", is the language: `:in aux` and `:strings aux` on
const tables. Catwiz built at squeeze 10 of 10, about 5 KB of it string literals in content
tables. How it is made:

- The checker folds the value of such a const with `Context::aux_strings` set, so each string
  literal in it becomes a table from a second pool (`str#a0`, ...) with `Placement::in_aux`; the
  const itself gets `in_aux` for `:in aux`. From there the layout, the link's aux segments and
  the interpreter's aux image treat them as they treat lets `:in aux`; the mirroring of const
  data with_aux code reads leaves them out (they are only in aux memory), and the link never
  makes one the interpreter's only (an address inside the code would be main memory's).
- Where a read by name is allowed (check/aux_consts.cpp): AuxModes has, beside the switches
  that may be on, the switches surely on (check/aux_mode.cpp): met where paths join, an
  AuxSwitch on gives the bools that are the constant true, and a function's entry is the meet
  over all its calls (0 for main, the handler and a function nothing calls; all on to start,
  then only shrinking, so the existing rounds settle it). A read by name needs RAMRD surely on,
  or must be a scalar `ConstValue` folds, which the backend does at -O0 too.
- The interpreter reads a place rooted at an `:in aux` var from its aux image (`Cursor::aux`).
- `ConstValue` does not fold a load through a constant pointer into aux data: outside a region
  the machine reads main memory there, and the interpreter does the same.
- The mid-level passes already treat `in_aux` vars as hardware (never steady, never hoisted,
  never kept in a local), which is what a read by name in a region needs.
- The cost is the `with_aux` around each read and the mirrored code that does it. 30 items with
  two strings (1.2 KB of text): 1067 bytes of main memory saved with `:in aux`, 956 with
  `:strings aux` (the table's 5 bytes an item stay); 40 cycles more a character read one at a
  time through a helper, 18 more copied a string at a time.

## Target

NMOS 6502 (runs on every Apple II), Apple IIe memory map, the extended
80-column card's aux memory and the language card when a program uses them.
Output is a raw binary (default origin `$6000`, for `BRUN` under DOS) or a
self-booting disk with no DOS (docs/disks.md). 65C02 (enhanced IIe, IIc) as a
later flag; its instructions and timings differ and must not be mixed up.
ProDOS SYS files later.

## Implementation

C++20 in the splonks-cpp / Adventures with Chickens style: C+- code, small
files by domain, splonks `.clang-format`, see `AGENTS.md`. Layout:

```text
src/main.cpp        tisple command and its subcommands (`tisple help`)
src/reader/         tokens, tree, formatter, highlighting, editor grammars
src/check/          macro expansion, names, types, layout facts
src/macro/          macro code: the compile-time evaluator macros run in
src/interp/         reference interpreter (the correctness oracle)
src/opt/            mid-level passes: constants, dead code, CSE, hoisting, inlining (docs/midlevel-optimizer.md)
src/backend/        isel, register/zero-page allocation, frames, peephole, size budget
src/link/           placement, constraint checks, binary output
src/asm/            6502 opcodes and the assembler the backend emits through
src/gaps/, src/superopt/  profiler and short-sequence search (`tisple gaps`)
src/ide/            names, types and errors for editors (`tisple index`)
src/race/           random race programs written in Tisple and C
src/emu/            cycle-exact 6502 core and test harness
src/machine/        built-in Apple IIe: soft switches, video, sound, mouse, Mockingboard, window
src/disk/           disk images, DOS 3.3 files, the self-booting disk's loader
tests/              unit, round-trip, fuzz
races/              drag racing against other compilers
web/                the machine as WebAssembly, for phones and the web demo
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
- Full-machine tests run headless on the built-in IIe (`tisple machine`,
  `tisple run --frames`): startup, text, lo-res, hi-res, speaker, disk boots,
  a2audit (docs/disks.md).

## Drag racing

Contestants: cc65, LLVM-MOS, Oscar64, Tisple, and handwritten assembly as the
time to beat. Results and settings: docs/races.md. Events: 16-bit multiply, memcpy,
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
4. Checker and reference interpreter for the core subset (done).
5. Plain backend: compiled programs run in the simulator and match the
   interpreter under fuzzing (done; `scripts/fuzz_farm.sh`).
6. Field-split struct arrays, static frames, zero-page allocation (done).
7. Computing macros (compile-time evaluation): tables, sprites, generics, unrolling (done).
8. `--cycles` reports (not yet; `tisple gaps` profiles instead).
9. First race against cc65, LLVM-MOS and Oscar64 (done: 13 kernels, `docs/races.md`).
10. Full Apple II: a hi-res demo booting in a headless emulator (done: the
    built-in IIe, self-booting disks, programs/paint).

## Open decisions

- Literal typing details, pointer arithmetic, signed compares.
- View syntax.
- How backing storage for allocators is declared.
- Calling convention for recursion (function pointers and interrupts: see
  Backend).
- IR shape.
