# Tisple ][ language: first subset

Started: 2026-10-26. Settled unless marked *proposed*.

Rule (owner, 2026-10-26): minimal. The goal is a compiler that is cheap to
write and produces very fast Apple II code, not a pleasant general language. A
feature earns its place only if it is cheap to compile or gives the optimizer
or linker facts. No conveniences for their own sake.

Most Tisple code will be written by Claude, so spellings come from Clojure or
Rust where one exists, each thing has exactly one way to write it, and every
diagnostic says what to write instead (see Diagnostics).
This is the subset the checker, reference interpreter and first backend build
against. Grow it when real programs need more.

## Program

A program is one or more `.tsp` files of top-level forms: `import`, `const`,
`let`, `struct`, `enum`, `fn`, `macro`. Order does not matter at top level.
`main` in the entry file is the entry point.

```scheme
(const MAX_SPRITES u8 32)

(struct Sprite (x u8) (y u8) (active bool))

(let sprites (array Sprite MAX_SPRITES))
(let sprite_count u8 0)

(fn spawn (args (x u8) (y u8)) (returns bool)
    (if (= sprite_count MAX_SPRITES)
        (return false))
    (set (at sprites sprite_count) (Sprite :x x :y y :active true))
    (set sprite_count (+ sprite_count 1))
    true)
```

## Files and modules

Multi-file programs are core (owner, 2026-10-26). The compiler always sees the
whole program, so files are namespaces, not separate compilation units.

```scheme
; gfx/hgr.tsp
(fn plot (args (x u16) (y u8))
    ...)

; main.tsp
(import gfx/hgr)
(import sound/speaker spk)

(fn main
    (hgr.plot 10 20)
    (spk.click))
```

- Import paths are relative to the project root (the entry file's folder),
  then `lib/` for the bundled Apple II library. The last segment is the
  prefix (`hgr.plot`); an optional second argument renames it.
- `tisple build main.tsp` follows imports to find every file.
- Every top-level name is importable for now.
- An import cycle is an error.
- A local may not have the same name as an imported module, so `hgr.x` can
  only mean one thing.

### lib/apple2

Every hardware address under its name from Apple's manuals (`KBD`, `KBDSTRB`,
`SPKR`, `TXTCLR`, `HIRES`, ...), plus the screen pages and the key codes
`(& (io_read u8 KBD) 0x7F)` gives (`KEY_LEFT`, `KEY_RIGHT`, `KEY_UP`, `KEY_DOWN`,
`KEY_RETURN`, `KEY_ESC`, `KEY_SPACE`, `KEY_DELETE`). Write `(import apple2 a2)`
and `a2.SPKR`; never type an I/O address or key code by hand.

## Keywords

`:name` does two things only:

1. Placement options on a top-level `let` or a `const` table, after the value:
   `:address`, `:align`, `:boundary` (later `:in`).
2. Field names in struct literals: `(Sprite :x 10 :y 20 :active true)`. Every
   field, in declared order, none left out. The names exist so a swapped
   `x`/`y` is an error instead of a silent bug; there is no reordering and no
   defaults.

Not values, not named arguments.

## Types

| Type | Meaning |
| --- | --- |
| `u8` `i8` `u16` `i16` | integers; arithmetic wraps |
| `bool` | `true` / `false`; not an integer |
| `(ptr t)` | 16-bit address of a `t` |
| `(array t n)` | `n` elements of `t`; `n` a constant |
| struct name | a declared `struct` |
| enum name | a declared `enum`; stored as `u8` |

Naming follows Rust (owner, 2026-10-26): types `PascalCase` (`Sprite`,
`Action`), constants `SCREAMING_SNAKE_CASE` (`MAX_SPRITES`), everything else
`snake_case`. Primitives stay `u8`, `i16`, `bool`. The checker enforces it.

## Integers

- No implicit widening or promotion. Both sides of `+`, `=`, `<` and so on must
  have the same type. `(+ (u16 a) b)` widens `a` explicitly.
- Conversions: `(u8 x)`, `(i8 x)`, `(u16 x)`, `(i16 x)`. Narrowing truncates;
  widening zero-extends from unsigned and sign-extends from signed. An enum
  member converts to its number, `(u8 Action.fire)`; nothing converts back to an
  enum. Pointers convert only with `as`, so `(ptr ...)` is always a type.
- A literal has no type of its own. It takes the type its context needs and is
  an error if out of range (`(+ x 300)` with `x : u8` fails). A literal with no
  context is an error: `(let n 5)` must be `(let n u8 5)`, and the same for
  `const`. So is a `for` over two literals: `(for y u8 0 48 ...)`, not
  `(for y 0 48 ...)` (owner, 2026-10-26).
- Arithmetic wraps: `(+ 255 1)` at `u8` is `0`. Comparisons are signed for
  `i8`/`i16`, unsigned otherwise.
- `*` exists at every width and compiles to a routine; `/` and `%` likewise,
  later.
- Bit operators are symbols, like arithmetic: `(& a b)`, `(| a b)`, `(^ a b)`,
  `(~ a)`, `(<< x n)`, `(>> x n)`. `>>` is logical for unsigned, arithmetic for
  signed. The shift count `n` is always `u8`, whatever the type of `x`; shifting
  by the width or more gives 0 (all sign bits for a signed `>>`).

## Forms

| Form | Meaning |
| --- | --- |
| `(const name type value)` | compile-time constant; `type` optional |
| `(const NAME (array t n) v0 v1 ...)` | constant table: exactly `n` values straight after the type (settled 2026-10-26) |
| `(let name type value)` | variable; `type` may be omitted. A local always has a value; a top-level `let` without one starts at zero in the image |
| `(set place value)` | assign to a name, `s.x`, `(at ...)` or `(. ...)` |
| `(fn name (args (p t) ...) (returns t) body ...)` | function; `args` omitted when none, `returns` omitted when it returns nothing |
| `(if c then else)` | both arms one form; with `else` it has a value, without it is a statement |
| `(do body ...)` | group forms; value is the last |
| `(while c body ...)` | loop |
| `(for i type start end body ...)` | `i` from `start` up to, not including, `end`; `type` optional, so `(for i u8 0 256 ...)` covers every byte |
| `(match x (pattern form) ...)` | first matching clause; patterns are literals, named constants (`a2.KEY_ESC`), enum members, or `_` |
| `(return value)` / `(return)` / `(break)` / `(continue)` | early exits |
| `(and a b)` `(or a b)` `(not a)` | short-circuit, on `bool` |
| `(at arr i)` | array element; `i : u8` when the array has at most 256 elements |
| `s.x` / `s.pos.x` | struct field of a named value |
| `(. expr name ...)` | struct field of an expression, `(. (at sprites i) x)`; only when the base is not a plain name |
| `(load p)` / `(store p v)` | read / write through a pointer; the only way to do either |
| `(load p x)` / `(store p x v)` | a field of the struct `p` points to; chains like `(load p pos x)`. `(. (load p) x)` is written `(load p x)` |
| `(addr_of place)` | address of a variable, element or field |
| `(as (ptr t) n)` | the `u16` address `n` as a `(ptr t)`, and back with `(as u16 p)` |
| `(io_read t addr)` / `(io_write t addr v)` | hardware access, `t` is `u8` or `u16` (low byte first); never dropped, merged or reordered |

A function returns a value only if it declares `(returns t)`. Its value is
then its last form or a `(return v)`. Without `returns`, the last form's value
is dropped and `(return)` takes no value. Calls always use parentheses,
`(read_key)`; only the declaration omits an empty `args`.

A dotted name means "look inside": a module (`hgr.plot`), an enum
(`Action.fire`) or a struct value (`dot.x`). The checker resolves the first
segment. Writing `(. dot x)` for a plain name is an error that says to write
`dot.x`. Code a macro made is exempt, since a template cannot glue `,s` and
`.x` into one symbol: templates write `(. ,s x)` (owner, 2026-10-26).

Built-in names (`at`, `load`, `addr_of`, `as`, ...) cannot be used for
variables, functions or fields; the error says to rename.

Locals declared with `let` inside a body live to the end of the enclosing body.
No shadowing in the same body.

## Enums and match

```scheme
(enum Action move_left move_right fire)

(fn handle (args (a Action))
    (match a
        (Action.move_left (step 255))
        (Action.move_right (step 1))
        (Action.fire (do
            (shoot)
            (play_click)))))
```

- Members are always written qualified, `Action.fire`, in patterns too.
- Members number from 0 in order. `(enum Action (fire 7) ...)` sets a value.
- `match` on an enum without `_` must name every member. On integers it needs
  `_`. Like `if`, it has a value when every clause gives one.
- Payload enums (Dudu-style) are later.
- Dense patterns compile to a jump table (push-address + `RTS` on NMOS,
  `JMP (table,X)` on 65C02); sparse ones to compare chains. The backend picks
  by size and cycles.

## Macros

Common Lisp-style templates, decided 2026-10-26. Expansion is tree to tree and
runs before the checker, so nothing later sees a macro.

```scheme
(macro swap (args a b)
    `(do
        (let tmp# ,a)
        (set ,a ,b)
        (set ,b tmp#)))
```

- `` ` `` quasiquote, `,` unquote, `,@` splice, `'` quote.
- A symbol ending in `#` inside a template becomes a fresh unique name on
  every expansion (from Clojure), so a caller's own `tmp` is never captured.
- Arguments are unevaluated forms. First version: the body is one template.
- Names the template writes resolve in the macro's own module, so a macro in
  `gfx/hgr` can call `plot` and mean `hgr.plot` wherever it is used. Unquoted
  arguments resolve where the macro is called (owner, 2026-10-26).
- `tisple expand file.tsp` prints the expanded program through the formatter.

### Next: computing macros

Planned right after the first backend (owner, 2026-10-26). A macro body runs
in the compiler and returns the forms to paste in. Its values are integers of
any size, symbols and lists, with a few list operations (`list`, `repeat`,
`map`, `range`, ...). Recursion is fine there, since it never runs on the
Apple II. One feature instead of separate table, unroll and generics features.
This is the old Lisp trick of writing the fast code as a generator: the
program says what to compute, the macro writes out the straight-line code.

Two levels, one syntax: code that runs on the Apple II is typed, static and
non-recursive, because tags, a heap, closures and deep stacks are what make
dynamic languages slow on a 1 MHz 6502. Code that runs in the compiler (macro
bodies) uses the same forms, `fn`, `let`, `if`, `match`, with relaxed rules:
any-size integers, lists, symbols, recursion, no annotations needed. A plain
Tisple function like `row_start` can be called from a macro at compile time and
also compiled for the Apple II.

```scheme
(macro unroll (args n body)
    `(do ,@(repeat n body)))

(macro row_table (args name)
    `(const ,name (array u16 192) ,@(map row_start (range 0 192))))
```

What it is for:

1. Lookup tables: hi-res row addresses, `x / 7` and `x % 7`, masks, sine
   tables, squares tables for fast multiply.
2. Pre-shifted hi-res sprites: the 7 shifted copies, generated from one drawing.
3. Graphics, fonts and tiles from ASCII art.
4. Music: note names to period and duration tables for the speaker.
5. Text: strings to Apple II screen bytes (bit 7 set), so no string type.
6. Generics: fixed-capacity pools and lists for any struct, e.g.
   `(pool Bullet 16 (x u8) (y u8))` gives the struct, array, count, spawn and
   despawn.
7. Unrolling: clears, copies and blits written as loops, emitted straight.
8. Specialized copies of a routine, e.g. one `plot` per color.
9. Flag constants and enums from one list.
10. Level and map data from text.

Not macros: exact cycle delays need a backend primitive (`wait_cycles`),
because a macro emits Tisple, not instructions.

## Memory and calls (first backend)

- Globals get fixed addresses from the linker. The interpreter uses the same
  layout, so pointer programs agree byte for byte.
- No recursion in this subset: the checker rejects call-graph cycles. Every
  function gets a static frame; for now the layout gives each function its own,
  the backend may later overlap frames that can't be live at the same time, and
  the interpreter follows whatever the layout says.
- Return value: `u8`/`i8`/`bool` in A, 16-bit in A (low) and X (high).
- Arrays of structs are stored one array per field, and arrays of `u16`, `i16`
  and pointers as a low-byte table and a high-byte table, unless something
  takes an element's address; then they are packed (little-endian). Either way
  `(at arr i)` with `i : u8` is one or two `LDA abs,X`.
- `(ptr t)` arithmetic: `(+ p n)` moves by `n` elements, not bytes (settled).

## Diagnostics

Every error gives file:line:column, the offending form, and the fix as code:

```text
main.tsp:12:9: u8 + u16: types must match; write (+ (u16 a) b)
main.tsp:4:1: this ( is never closed; its form starts here
main.tsp:20:5: let declares a variable here, it has no body; put the body after it
main.tsp:7:5: (Sprite :y 1 :x 2 ...) fields out of order; write (Sprite :x 2 :y 1 ...)
```

`tisple build --asm` prints each function's generated 6502 code under its
source line; `tisple expand` prints the program after macro expansion.

## Placement

In the first subset; the linker checks them and fails the build if one can't
hold. Named regions (`:in`) come later.

| Option | Meaning |
| --- | --- |
| `:address 0x6000` | start at exactly this address |
| `:align 256` | start on a multiple of 256 |
| `:boundary 256` | whole object inside one 256-byte block; for a split table, each byte table |

Placement goes on top-level `let`s and `const` tables. An explicit `:address`
may point anywhere, a hi-res page included (`(let screen (array u8 8192)
:address 0x2000)`); automatic placement never uses zero page, the stack, text
page 1, hi-res page 1 or `$C000` and up.

## Planned after the first backend (owner, 2026-10-26)

- Function pointers, no closures: dispatch tables, per-object update
  functions. Cheap on the 6502 (indirect `JMP` or push-address + `RTS`).
- Tail calls: a call in tail position compiles to `JMP`, not `JSR` + `RTS`.
  Backend only, no syntax.

Ideas, in order of cost, taken up when real programs need them:

1. Tail recursion as loops: a self-call in tail position jumps to the top, no
   frame. Comes free with tail calls, so the no-recursion rule can relax for it.
2. Recursion at compile time: pure functions called with constant arguments
   run in the interpreter while compiling (`(fib 10)` becomes `55`). Comes
   with computing macros.
3. Downward-only lambdas: a lambda passed to a call and never stored is
   compiled by specializing the callee per call site with the body inlined;
   captured variables are slots in the caller's static frame. Overlaps with
   computing macros; macros first, lambdas only if those prove clumsy.
   Stored (escaping) closures need a heap and stay out.

Bounded recursion is not a feature: write the loop with an explicit stack
(fixed arrays plus a counter), which compiles to the same indexed code. A
computing macro can generate the stack if it gets repetitive (owner,
2026-10-26).

## Out of the first subset

Named regions, strings, general recursion, inline assembly. `/` and
`%` pass the checker and interpreter; the backend gets them later. All planned;
none needed to prove the backend.

## Pending change (owner, 2026-10-26)

Applied once the backend branch lands, so it doesn't conflict with it:

- The parameter list is always written, `()` when empty, and `args` goes away:
  `(fn flip ((x u16) (y u8)) void ...)`, `(macro inc (place) ...)`.
- The return type is always the third item, `void` when nothing is returned,
  and `(returns t)` goes away: `(fn read_key () u8 ...)`.
- `as` is the only conversion: `(as u16 y)`, `(as u8 addr)`,
  `(as (ptr u8) addr)`, `(as u8 action)`. `(u8 x)`-style conversions go away.
- Struct literals are `(make Sprite :x 10 :y 20 :active true)`. With this and
  `as`, a type never heads a form: the first word of every form is a special
  form, built-in, macro or function, so macros and tools can tell any form
  apart by its head alone.
- Declarations always have the form name, type, value: `let`, `const` and `for` always
  state the type: `(let key u8 (io_read u8 a2.KBD))`, `(for i u8 0 n ...)`.
- `make` builds every compound value, arrays too, so a value slot always holds
  one form: `(const ROW_ADDR (array u16 192) (make (array u16 192) 0x2000 ...))`.
- Field and member lists look like parameter lists:
  `(struct Sprite ((x u8) (y u8) (active bool)))`, `(enum Action (none left right))`.
- `match` clauses take a body like `fn`/`while`/`for`, so `(_ (do))` is `(_)`.
  Only `if`'s two arms are single forms.
- Access paths: `(at arr i path...)` like `(load p path...)`, e.g.
  `(at sprites i x)`. Hand code uses `name.x`, `at` or `load`; `(. base x)`
  stays only for macro templates.
- Shapes after this batch: declare `(kind name type value options...)`, build
  `(make Type ...)`, access (`name.x`, `at`, `load`), control
  `(form head... body...)` with `if` the one two-arm exception. Stop there:
  fewer shapes would force different meanings into one.
- Declarations stay `(let dog u8 5)`; `(u8 dog 5)` would clash with struct
  literals and make declarations hard to search for.
- No multiple return values for now. The backend may return a `bool` in the
  carry flag, the ROM's own convention, with no syntax change.

## Settled 2026-10-26

- Array literals: `(const ROW_ADDR (array u16 192) 0x2000 0x2400 ...)`, values
  straight after the type.
- `(. name x)` is an error in hand-written code (write `name.x`) but allowed in
  macro output, since a template cannot glue `,s` and `.x` into one name.
