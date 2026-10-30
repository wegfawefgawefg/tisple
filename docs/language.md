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

(struct Sprite ((x u8) (y u8) (active bool)))

(let sprites (array Sprite MAX_SPRITES))
(let sprite_count u8 0)

(fn spawn ((x u8) (y u8)) bool
    (if (= sprite_count MAX_SPRITES)
        (return false))
    (set (at sprites sprite_count) (make Sprite :x x :y y :active true))
    (set sprite_count (+ sprite_count 1))
    true)
```

## Shapes

Every form has one of four shapes (owner, 2026-10-26), and its first word is
always a special form, built-in, macro or function, never a type, so macros and
tools can tell any form apart by its head alone:

1. Declare: `(kind name type value options...)`: `let`, `const`, and `for`
   with its range. A value slot always holds exactly one form.
2. Build: `(make Type ...)` for struct and array values.
3. Access: `name.x`, `(at arr i path...)`, `(load p path...)`.
4. Control: `(form head... body...)`: `fn`, `macro`, `while`, `for`, `do`,
   `match` and its clauses. `if` is the one exception: its two arms are single
   forms.

Stop there: fewer shapes would force different meanings into one.
Declarations stay `(let dog u8 5)`; `(u8 dog 5)` would clash with the build
shape and make declarations hard to search for.

## Files and modules

Multi-file programs are core (owner, 2026-10-26). The compiler always sees the
whole program, so files are namespaces, not separate compilation units.

```scheme
; gfx/hgr.tsp
(fn plot ((x u16) (y u8)) void
    ...)

; main.tsp
(import gfx/hgr)
(import sound/speaker spk)

(fn main () void
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

### lib/text80, lib/text40 and lib/auxmem

- `lib/text80`: 80-column text on the extended 80-column card. `(text80.on)`,
  `(text80.print "Hello")` (a macro: a `put` call a character),
  `(text80.put_str s)` for a zero-terminated string up to 255 long (a loop over
  it), `(text80.put c)` for one ASCII character, `(text80.put_code c)` for a screen
  code as it is (MouseText is `$40`-`$5F`), `(text80.move col row)`,
  `(text80.inverse true)`, `(text80.clear)`, `(text80.put_row r p)` for 80 codes
  at once, `(text80.off)`. Even columns are in aux memory: it writes them with
  80STORE and PAGE2 on, through `io_write` so nothing moves across the switch.
- `lib/text40`: the same on text page 1, 40 columns, without the aux half.
- `lib/auxmem`: `(auxmem.read dst src count)` and `(auxmem.write dst src
  count)` copy between main memory and aux memory, with pointers from
  `addr_of`. It is the only way code reaches a let `:in aux` (see Memory map).
  Its mover is 42 bytes of our own 6502 code run from page 3, with a copy at the
  same address in aux memory, since RAMRD sends the instruction fetches there
  too; `:reserve` lets keep `$0300`-`$03AB` in main memory and all of page 3 in
  aux memory, so an `:address` let there is an overlap error. Its arguments are
  four lets at `$F9`-`$FE`, which the program's other variables never take. A
  one-byte read takes about 140 cycles with its call (560 before 2026-10-30,
  when the mover saved the zero page it borrowed), a page about 4100 more.
  `(auxmem.guard)` makes copies safe while an interrupt handler runs
  (Interrupts). (Not `aux.tsp`: Windows keeps that name.)

## Keywords

`:name` does two things only:

1. Placement options on a top-level `let` or a `const` table, after the value:
   `:address`, `:align`, `:boundary`, and on a `let` `:in aux` and `:reserve`
   (see Placement). `:reserve` is the one option without a value. On a `fn`,
   after the return type, `:interrupt` (see Interrupts), also without one.
2. Field names in struct values: `(make Sprite :x 10 :y 20 :active true)`.
   Every field, in declared order, none left out. The names exist so a swapped
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
| `(fn (t ...) r)` | address of a function taking `t ...` and giving `r` (`void` for nothing); see Function pointers |
| struct name | a declared `struct` |
| enum name | a declared `enum`; stored as `u8` |
| `void` | only as the return type of a `fn` that returns nothing |

Naming follows Rust (owner, 2026-10-26): types `PascalCase` (`Sprite`,
`Action`), constants `SCREAMING_SNAKE_CASE` (`MAX_SPRITES`), everything else
`snake_case`. Primitives stay `u8`, `i16`, `bool`. The checker enforces it.

## Integers

- No implicit widening or promotion. Both sides of `+`, `=`, `<` and so on must
  have the same type. `(+ (as u16 a) b)` widens `a` explicitly. The one
  exception is a pointer's count: `(+ p x)` takes a `u8` or `u16` `x`.
- `as` is the only conversion: `(as u16 y)`, `(as u8 addr)`. Narrowing
  truncates; widening zero-extends from unsigned and sign-extends from signed.
  An enum member converts to its number, `(as u8 Action.fire)`; nothing
  converts back to an enum, and nothing converts to or from `bool`. A pointer
  converts only to and from `u16`: `(as (ptr u8) addr)`, `(as u16 p)`.
- A literal has no type of its own. It takes the type its context needs and is
  an error if out of range (`(+ x 300)` with `x : u8` fails). `let`, `const`
  and `for` always state the type, so a literal next to one always has one.
  Elsewhere a literal with no context is an error, such as `(= 1 2)`.
- Arithmetic wraps: `(+ 255 1)` at `u8` is `0`. Comparisons are signed for
  `i8`/`i16`, unsigned otherwise.
- `*`, `/` and `%` exist at every width and compile to routines (or shifts and
  masks for a power of two). `/` truncates toward zero and `%` takes the
  dividend's sign.
- Division by zero is defined as what the 6502 routine gives (owner reading,
  2026-10-26): `(/ x 0)` has every bit set (`255`, `65535`, or `-1` signed),
  except `1` for a negative signed `x`; `(% x 0)` is `x`. The interpreter gives
  the same and goes on. A division by zero in a constant is an error.
- Bit operators are symbols, like arithmetic: `(& a b)`, `(| a b)`, `(^ a b)`,
  `(~ a)`, `(<< x n)`, `(>> x n)`. `>>` is logical for unsigned, arithmetic for
  signed. The shift count `n` is always `u8`, whatever the type of `x`; shifting
  by the width or more gives 0 (all sign bits for a signed `>>`).

## Forms

| Form | Meaning |
| --- | --- |
| `(const NAME type value)` | compile-time constant, a scalar or a table of scalars |
| `(let name type value)` | variable. A local always has a value; a top-level `let` may leave it out |
| `(set place value)` | assign to a name, `s.x` or `(at ...)` |
| `(fn name (params) type body ...)` | function; `(params)` is `()` when empty, `type` is `void` when it returns nothing |
| `(if c then else)` | both arms one form; with `else` it can give a value, without it is a statement |
| `(do body ...)` | group forms; value is the last |
| `(while c body ...)` | loop |
| `(for i type start end body ...)` | `i` from `start` up to, not including, `end`; `(for i u8 0 256 ...)` covers every byte |
| `(match x (pattern body ...) ...)` | first matching clause; patterns are literals, named constants (`a2.KEY_ESC`), enum members, or `_` |
| `(return value)` / `(return)` / `(break)` / `(continue)` | early exits |
| `(and a b)` `(or a b)` `(not a)` | short-circuit, on `bool` |
| `(make Type ...)` | a struct value `(make Sprite :x 1 :y 2 :active true)`, or an array value `(make (array u8 3) 1 2 3)` |
| `(as type x)` | conversion, see Integers |
| `(at arr i)` / `(at arr i x)` | array element, and a field path of it like `(at sprites i pos x)`; `i : u8` when the array has at most 256 elements |
| `s.x` / `s.pos.x` | struct field of a named value |
| `(load p)` / `(store p v)` | read / write through a pointer; the only way to do either |
| `(load p x)` / `(store p x v)` | a field of the struct `p` points to; chains like `(load p pos x)` |
| `(addr_of place)` | address of a variable, element or field |
| `(io_read t addr)` / `(io_write t addr v)` | hardware access, `t` is `u8` or `u16` (low byte first); never dropped, merged or reordered |
| `(jsr addr (a v) (x v) (y v))` | call machine code (ROM, card firmware) with registers set; gives A after; see Machine code |
| `name` of a function, `(f args ...)` with `f` holding one | a function's address; a call through it, see Function pointers |
| `(wait_cycles n)` | exactly n cycles (a variable n: 69 at least); see Exact delays |
| `(sei)` / `(cli)` | interrupts off / on; see Interrupts |
| `"text"` | a `(ptr u8)` to the text's ASCII bytes and a 0; see Strings |

An `io_read` or `io_write` at a constant address is one absolute instruction. So
is one at a u16 global plus or minus a constant, such as a card's slot page that
init finds: the compiler rewrites those instructions when the global is set
(docs/design.md, "I/O bases"), as long as the global has no `addr_of` and the
interrupt handler never sets it. At any other address an access costs about 16
cycles more.

The parameter list is always written, `()` when empty, and the return type is
always the third item, `void` when nothing is returned:
`(fn flip ((x u16) (y u8)) void ...)`, `(fn read_key () u8 ...)`. A function
with a return type gives its last form's value or a `(return v)`; a `void`
function drops its last form's value and `(return)` takes no value. Calls
always use parentheses, `(read_key)`.

Declarations always have the shape name, type, value: `(let key u8 (io_read u8
a2.KBD))`, `(const MAX u8 32)`, `(for i u8 0 n ...)`. A top-level `let` may
leave out the value, `(let sprites (array Sprite 32))`: it starts at zero,
except with `:address` (see Placement). A local `let` always has a value.

`make` builds every compound value, so a value slot always holds one form. A
struct value names every field in declared order. An array value gives exactly
`n` elements, each a constant; it is only the value of a `const` table or a
top-level `let`: `(const ROW_ADDR (array u16 192) (make (array u16 192) 0x2000
0x2400 ...))`. A `const` table holds numbers, bools, enum members or pointers;
a top-level `let` may also start an array of structs or of arrays. Arrays are
never copied as values, so other arrays are filled element by element.

`if` and `match` give a value only when it is used: as an operand, argument,
`let` or `set` value, return value or a function's last form. Then every arm
must give the same type, and bare literals take it from the other arms. An
`if` or `match` whose value is dropped is a statement, and its arms may have
different types, or none (owner reading, 2026-10-26); `do` likewise.

Access paths: `(at arr i path...)` reaches fields of array elements the way
`(load p path...)` reaches fields through a pointer, `(at sprites i x)`. Hand
code uses `name.x`, `at` or `load`. A dotted name means "look inside": a module
(`hgr.plot`), an enum (`Action.fire`) or a struct value (`dot.x`); the checker
resolves the first segment. `(. base x ...)` is only for macro output, since a
template cannot glue `,s` and `.x` into one symbol: templates write
`(. ,s x)` (owner, 2026-10-26). In hand code it is an error that says what to
write: `p.x`, `(at sprites i x)` or `(load p x)`.

Built-in names (`at`, `load`, `addr_of`, `as`, `make`, ...) cannot be used for
variables, functions or fields; the error says to rename.

Locals declared with `let` inside a body live to the end of the enclosing body.
No shadowing in the same body.

No multiple return values for now. The backend may return a `bool` in the carry
flag, the ROM's own convention, with no syntax change.

## Structs, enums and match

Field and member lists have the parameter-list shape: one list after the name.

```scheme
(struct Sprite ((x u8) (y u8) (active bool)))

(enum Action (none move_left move_right fire))
(enum Key ((esc 0x1B) (space 0x20) (delete 0x7F)))

(fn handle ((a Action)) void
    (match a
        (Action.none)
        (Action.move_left (step 255))
        (Action.move_right (step 1))
        (Action.fire
            (shoot)
            (play_click))))
```

- A field is `(name type)`.
- A member is `name` or `(name value)`, and the two may mix: the first member
  is 0 unless it gives a value, and a plain member is one more than the member
  before it. Values are 0 to 255 and no two members share one.
- Members are always written qualified, `Action.fire`, in patterns too.
- A clause is `(pattern body ...)`; its body is several forms with an implicit
  `do`, so `(_)` does nothing.
- `match` on an enum without `_` must name every member. On integers it needs
  `_`. Like `if`, it gives a value when that is used.
- Payload enums (Dudu-style) are later.
- Dense patterns compile to a jump table (push-address + `RTS` on NMOS,
  `JMP (table,X)` on 65C02); sparse ones to compare chains. The backend picks
  by size and cycles.

## Macros

A macro is code that runs in the compiler and gives the forms to paste in.
Templates were decided 2026-10-26, computing macros planned the same day
(owner) and built right after the first backend. Expansion is tree to tree and
runs before the checker, so nothing later sees a macro: the expanded code is
ordinary Tisple, checked like hand-written code, and `tisple expand file.tsp`
prints it through the formatter.

This is the old Lisp trick of writing the fast code as a generator: the
program says what to compute, the macro writes out the straight-line code. One
feature instead of separate table, unroll and generics features.

```scheme
(macro swap (a b)
    `(do
        (let tmp# u8 ,a)
        (set ,a ,b)
        (set ,b tmp#)))

; 128 + 127 sin(2 pi i / 256), rounded
(macro sine_byte (i)
    (round (+ 128 (* 127 (sin (/ (* (* 2 pi) i) 256))))))

; lib/tables
(macro table (name type count f & options)
    `(const ,name (array ,type ,count)
        (make (array ,type ,count) ,@(map f (range 0 count)))
        ,@options))

(tables.table SINE u8 256 sine_byte)
```

- `(macro name (params) body ...)`; the parameter list is `()` when empty.
  `(a b & more)` gives `more` the remaining arguments as a list (from
  Clojure): a macro that takes a body or rows takes them this way.
- The body is macro code, below. Its last form's value is the expansion, and it
  must be forms: a float, a fn or nothing is an error at the call. A body that
  is one quasiquote is a template and works exactly as templates always did.
- `` ` `` quasiquote, `,` drops in any value (`,(len rows)`), `,@` splices a
  list, `'` quote.
- A symbol ending in `#` inside a template becomes a fresh unique name on
  every call (from Clojure), so a caller's own `tmp` is never captured.
- Names a template writes resolve in the macro's own module, so a macro in
  `gfx/hgr` can call `plot` and mean `hgr.plot` wherever it is used. Argument
  forms resolve where the macro is called (owner, 2026-10-26). A symbol made
  with `symbol` resolves where its first part was written, so
  `(symbol name "_x")` names something next to the caller's `name`.

### Arguments: forms from code, values from macro code

A macro called from Tisple code gets the argument forms, unevaluated. A macro
called from macro code gets the argument values. Forms are values (symbols,
integers, strings, keywords and lists of them), so it is one rule: the
parameters hold what the caller had.

So a compile-time helper is a macro too, and there is no separate helper form:
`sine_byte` above is called by `map` with integers, and written in code,
`(sine_byte 64)` pastes `255`. A `macro_fn` form was the alternative; it would
be a second way to write a compile-time function that differs from a macro
only in how arguments arrive, which the one-way rule rules out
(implementation, 2026-10-26).

### Macro code

Two levels, one syntax. Code that runs on the Apple II is typed, static and
non-recursive, because tags, a heap, closures and deep stacks are what make
dynamic languages slow on a 1 MHz 6502. Macro code runs in the compiler and
uses the same forms with relaxed rules: no types, 64-bit integers, floats,
lists, symbols, closures and recursion.

| Form | Meaning |
| --- | --- |
| `(let x value)` / `(set x value)` | a local and assignment; no type: `(let x u8 5)` says to write `(let x 5)` |
| `(if c then else)` `(do body ...)` `(while c body ...)` | as in Tisple; conditions are `true` or `false`, never numbers |
| `(for i start end body ...)` | `i` from `start` up to, not including, `end`; no type |
| `(match x (pattern body ...) ...)` | patterns are literals, `'symbol`, `true`, `false` and `_`; no match is an error |
| `(return v)` `(break)` `(continue)` `(and a b)` `(or a b)` `(not a)` | as in Tisple |
| `(fn (a b) body ...)` | a function value, no name and no types; it keeps the values it sees when made |
| `(macro_name args ...)` | a macro, with values |
| `(row_start 5)`, `MAX_X` | an ordinary Tisple `fn` or scalar `const`, see below |

Tisple-only forms (`const`, `make`, `at`, `as`, `load`, ...) in macro code are
errors that say to build them in a template: `` `(make ...) ``.

Values:

- Integers are 64-bit signed, and an operation that does not fit is an error,
  never a wrap (implementation, 2026-10-26: true big integers would need a
  library or a hand-written one, and 64 bits holds every Apple II number many
  times over). `/` truncates toward zero, `%` takes the dividend's sign, `>>`
  is arithmetic, and shift counts are 0 to 63. Division by zero is an error.
- Floats exist only at compile time, for tables like sines. `+ - * /` give a
  float when either side is one; `pi`, `sin`, `cos`, `sqrt` and `(float n)`
  make them. `floor`, and `round` (halves away from zero), give integers. A
  float in the expansion is an error that says to round it. The reader has no
  float literals: write `(/ 1 (float 2))`.
- `true` and `false` paste as themselves; symbols, keywords and lists paste as
  forms.
- Strings are values (ASCII art, text); one that reaches code is a string
  literal there (see Strings).
- `=` and `!=` compare any two values, lists item by item; `< <= > >=` compare
  numbers.

Library, in macro code only (Tisple code may still use these names):

| Function | Gives |
| --- | --- |
| `(list a ...)` | a list |
| `(len xs)` | items of a list, bytes of a string |
| `(nth xs i)` `(head xs)` `(tail xs)` | an item, the first, all but the first; past the end is an error |
| `(append xs ...)` | the lists joined |
| `(map f xs)` | `(f x)` for each item; `f` is a macro, a fn, a function's name, or a `(fn (i) ...)` form passed as an argument |
| `(range start end)` | integers from `start` up to, not including, `end`, like `for` |
| `(repeat n x)` | `n` copies of `x` |
| `(symbol part ...)` | a symbol of symbols, strings and integers joined: `(symbol 'sprite "_" 'x)` is `sprite_x` |
| `(chars "text")` | the character codes |
| `(kind x)` | `'int`, `'float`, `'bool`, `'symbol`, `'keyword`, `'string`, `'list` or `'fn` |
| `(error part ...)` | stops with this message, at the call |
| `sin cos sqrt floor round float pi` | see Values |

A list holds at most 131072 items. Parameters and lets may not take a library
name.

### Tisple functions and constants at compile time

Macro code can call an ordinary `fn` and read a scalar `const`. They run in
the reference interpreter with their declared types, so `(row_start 200)`
wraps exactly as on the Apple II, and the same function is also compiled for
it (`examples/dot/gfx/hgr.tsp`). Arguments must fit the parameter types: `300`
for a `u8` is an error that gives the range. Arguments and results are
integers, bools, and pointers as integers; enums are not supported yet.

Such a function may not use `io_read` or `io_write`, store through a pointer,
or read or set a top-level `let` (it has a value only at run time); calling
one that does is an error that says which. Constants, const tables and other
functions are fine.

The functions and constants are checked as a small program of their own, from
every top-level declaration their names reach. Two limits: a declaration a
top-level macro call makes (a `tables.table`) is not part of it, and a
function used this way may not itself use a macro that makes a compile-time
call.

### Limits and errors

One call from Tisple code may run 10,000,000 steps (every evaluated form, and
every interpreter step of a typed call) and nest 200 calls deep. Past that the
error is at the call and names the macro:

```text
main.tsp:4:1: macro spin ran more than 10000000 steps; is a loop missing its end? (in macro spin)
main.tsp:6:1: macro deep nested more than 200 calls deep; does its recursion reach a base case? (in macro deep)
```

Every error in macro code is at the call, names the macro, and says where in
the macro code it happened; `(error ...)` gives just its message:

```text
main.tsp:9:1: + takes integers, got the symbol a; macro code at main.tsp:3:5 (in macro scale)
main.tsp:12:1: sprite BALL: row 1 is 3 wide, the first is 4 (in macro tables.sprite)
```

### In lib

- `lib/tables`: `(tables.table NAME type count f options ...)` is
  `(const NAME (array type count) (make (array type count) (f 0) (f 1) ...)
  options ...)`. `(tables.sprite NAME row ...)` turns rows of `#` and `.` into
  hi-res bytes: 7 pixels a byte, the low bit leftmost, `(width + 6) / 7` bytes
  a row.
- `lib/macros`: `inc`, `dec`, and `(macros.unroll i start end body ...)`: the
  body once for each `i`, with `i` a literal in each copy, each copy in its own
  `do`. Plasma's draw loop unrolled goes from 18.3 to 20.1 frames a second,
  for 1 KB of code (`docs/benchmarks.md`).

What macros are for:

1. Lookup tables: hi-res row addresses, `x / 7` and `x % 7`, masks, sine
   tables, squares tables for fast multiply.
2. Pre-shifted hi-res sprites: the 7 shifted copies, generated from one drawing.
3. Graphics, fonts and tiles from ASCII art.
4. Music: note names to period and duration tables for the speaker.
5. Text: strings to Apple II screen bytes (bit 7 set), so no string type.
6. Generics: fixed-capacity pools and lists for any struct, e.g.
   `(pool Bullet 16 ((x u8) (y u8)))` gives the struct, array, count, spawn and
   despawn.
7. Unrolling: clears, copies and blits written as loops, emitted straight.
8. Specialized copies of a routine, e.g. one `plot` per color.
9. Flag constants and enums from one list.
10. Level and map data from text.

Not macros: exact cycle delays need a backend primitive (`wait_cycles`),
because a macro emits Tisple, not instructions.

## Memory and calls (first backend)

- Globals get fixed addresses from the linker. `tisple check`, `interp`,
  `build` and `run` all lay the program out the way the backend does, so
  pointer programs agree byte for byte and layout errors are the same
  everywhere. `check` takes the build's `--origin A`, `--disk` and code
  flags for that.
- An `addr_of` into an array of multibyte elements stores the whole array
  packed (pointers see packed bytes), so every access to it multiplies the
  index by the element size. build, run and check warn at the first such
  `addr_of`.
- No recursion in this subset: the checker rejects call-graph cycles. A call
  through a function pointer counts as a call of every function of its type
  whose name is used as a value. Every function gets a static frame; the
  backend overlaps frames that can't be live at the same time, and the
  interpreter follows whatever the layout says.
- Return value: `u8`/`i8`/`bool` in A, 16-bit in A (low) and X (high).
- Arrays of structs are stored one array per field, and arrays of `u16`, `i16`
  and pointers as a low-byte table and a high-byte table, unless something
  takes an element's address; then they are packed (little-endian). Either way
  `(at arr i)` with `i : u8` is one or two `LDA abs,X`.
- `(ptr t)` arithmetic: `(+ p n)` moves by `n` elements, not bytes (settled).
  `n` is `u8` or `u16`; a `u8` count is `(as u16 n)` written short, since
  `(load (+ p x))` with `x : u8` is the 6502's `LDA (p),Y` (2026-10-26).

### Pointer provenance

Owner reading, 2026-10-26; the backend relies on it to keep values in
registers across stores. A pointer reaches only what it was made from:

- A pointer from `addr_of` reaches only inside that variable. Moving it with
  `+` or `-` past the variable's bytes and using it is undefined.
- A pointer made with `as` from an integer reaches only memory the program does
  not declare: screens, soft switches, ROM, buffers at addresses no variable
  uses.

So a store through a pointer can only change variables whose address is taken.
The layout checks the one case it can see: a constant `(as (ptr t) n)` whose
`n` falls inside a declared variable is an error that says to use `addr_of`.
Declare the screen as a variable and write `(addr_of (at screen i))`, or leave
it undeclared and use `as`; not both.

## Function pointers

Added 2026-10-30 (owner: dispatch tables and per-object update functions, no
closures).

```scheme
(fn walk ((i u8)) void ...)
(fn fall ((i u8)) void ...)
(const STEPS (array (fn (u8) void) 2) (make (array (fn (u8) void) 2) walk fall))

(struct Thing ((x u8) (step (fn (u8) void))))
(let things (array Thing 8))

(set (at things 0 step) (at STEPS 1))
(let step (fn (u8) void) (at things i step))
(step i)
```

- The type is `(fn (t ...) r)`: the parameter types, then the return type,
  `void` for nothing; the declaration `(fn name (params) type ...)` with the
  name and parameter names left out. Types with the same parameter and return
  types are the same type. 2 bytes, like a pointer; arrays of them are a
  low-byte and a high-byte table.
- A function's name used as a value is its address: `(let f (fn (u8) void)
  walk)`. `(addr_of walk)` is an error that says to write `walk`.
- `(f args ...)` calls through `f` when `f` names a local, parameter, global or
  field (`thing.step`) holding one. Anything else, an element or a load, goes
  through a `let` first: the head of a form is always a name. Arguments are
  checked against the type's parameters; the callee is worked out before the
  arguments.
- A const table may hold functions' names. A top-level `let` may not start
  with them (its bytes are worked out before the code is placed): set it in
  code. A `const` scalar of a function type is an error that says to name the
  function directly.
- `=` and `!=` compare them. Nothing converts to or from one, and there is no
  arithmetic on them, so a call through a pointer of type t can only reach a
  function of type t whose name is used as a value somewhere in the program:
  those are its targets, and the checker counts them as called from there
  (recursion through a pointer is an error like any other).
- Calling a pointer that holds no function (a zeroed global) is undefined; the
  interpreter stops with "holds no function of its type".
- Not for compile-time calls from macros, which say "uses a function pointer".

What it compiles to (design.md, "Calls through function pointers"): the call
stores the pointer and the arguments into a dispatcher of the type, a function
shared by every call of that type, and JSRs to it; the dispatcher stores the
pointer into its own `JMP` and jumps. The pointer holds the address of the
target's entry, a small function that calls the target with the dispatcher's
arguments (usually the target's body inlined). About 30 cycles more than a
direct call, plus a copy of each argument when the target is not inlined.

What the optimiser assumes of a function whose address is taken: it is called
from anywhere a pointer to it goes. Its parameters get no ranges from the calls
the tree shows, its return values are kept even when no direct call uses them,
and its pointer parameters reach anything. A call through a pointer reads and
writes what any of its targets may (their effect summaries joined), and ends
only if each does. Inlining and called-once decisions see the entry's call of
the target as a call like any other.

## Interrupts

Added 2026-10-30, for the Mockingboard's timers and the mouse card.

```scheme
(let ticks u16 0)

(fn tick () void :interrupt
    (io_read u8 T1_LOW) ; answers the 6522
    (set ticks (+ ticks 1)))

(fn main () void
    ...
    (sei)
    (let now u16 ticks) ; both bytes from one moment
    (cli)
    ...)
```

- `(fn name () void :interrupt body ...)` is the program's interrupt handler: it
  runs on every IRQ, between any two instructions of the rest. One a program, no
  parameters, no value; the program needs a main. Calling it or using its name
  as a value is an error.
- The startup installs it before main with interrupts off, turns them on for
  main (`CLI`), and at the end takes it out with them off and gives the caller
  its I flag back. The device is the program's to start and stop: write its
  registers with `io_write`, answer it in the handler.
- `(sei)` and `(cli)` turn interrupts off and on: the 6502's own instructions,
  as `jsr` is. They are effects like `io_write`: never dropped or moved. A
  `(cli)` in the handler or anything it calls is an error (interrupts would nest
  over frames that have one copy); its `RTI` turns them on again.
- The handler shares no function with main's code: a function both reach is an
  error that says to give the handler a copy. Calls through function pointers
  count as calls of their targets here too.
- Reads tear: a `u16` or a struct the handler writes is read a byte at a time,
  and an interrupt between the bytes gives half old, half new. Read it between
  `(sei)` and `(cli)`. The same for a read-modify-write main and handler both
  make: `(set n (+ n 1))` is several instructions.
- What the optimiser assumes: every global the handler (or a function it calls)
  names, every variable whose address is taken when it goes through a pointer,
  and every global as well when it runs `jsr`, may change or be looked at
  between any two instructions. They count as hardware: no ranges, constants,
  dead stores, merged or hoisted reads, collapsed loops, and no register is
  taken to hold one. So `(while (= flag 0))` waits for the handler, and every
  `(set mode ...)` main makes is made. Everything else is optimised as before:
  the handler runs only its own code and frames.
- The interpreter takes interrupts only where a test asks for them (between two
  steps, when they are on).
- A `BRK` with the card's ram in runs the handler and returns past it; Tisple
  code has none. lib/auxmem's mover switches RAMRD and RAMWRT: on an
  unenhanced IIe an interrupt in it would fetch the vector, and with the card
  in the handler's code, from aux memory, and with RAMWRT on the handler would
  write its variables there. Either put `(sei)` and `(cli)` around
  `auxmem.read` and `auxmem.write` when a handler may run, or call
  `(auxmem.guard)` once in main: our code in page 3 that the vectors then go
  through, which runs the handler with main memory in and switches back after
  (19 cycles a tick, about 65 more for a tick that lands in a copy;
  docs/mockingboard.md). lib/mouse stays polled (its interrupt modes need
  SERVEMOUSE in the handler, which our machine's card does not raise).

What it compiles to, and how each Apple II routes the IRQ: design.md,
"Interrupt handlers".

## Machine code

Owner, 2026-10-26: programs call ROM and card firmware (the mouse first) with one form, and
there is no inline assembler.

```scheme
(jsr read_entry (x cn) (y n0)) ; READMOUSE with X = $Cn, Y = $n0
(jsr 0xFBDD)                   ; the monitor ROM's BELL1; its A is dropped
```

- `addr` is a `u16`. The register pairs are optional, each at most once, in the order
  `a`, `x`, `y`, each a `u8`. The value is A after the call, a `u8`; it may be dropped.
- The address, then the register values, are worked out in the order written; then A, X
  and Y are set and the JSR runs. A constant address is a plain `JSR`; a computed one is
  stored into the `JSR`'s own operand first (programs run from RAM).
- To the optimiser it is I/O and a store through an unknown pointer at once: never
  dropped, merged or moved, every register and flag clobbered, and what the code knew about
  registers forgotten.
- The routine may change any top-level `let`, any variable whose address is taken, and
  memory the program does not declare (screen holes, soft switches); it never changes a
  local whose address is not taken. It must return with `RTS`, the stack as it found it and
  decimal mode off, and leave zero page alone: Tisple keeps variables in all of it. So
  monitor routines that use zero page (`COUT` at `$FDED` among them) need care.
- The reference interpreter cannot run machine code: a `jsr` stops it with "jsr needs the
  machine", so a function that uses one cannot run at compile time, and the fuzz generator
  never writes one. Test such code in the emulator (`tisple run`).
- Spelling: the 6502's own mnemonic, since that is exactly what it compiles to; the
  registers are `(name value)` pairs because keywords are never named arguments (see
  Keywords), and their fixed order keeps one way to write each call.

## Diagnostics

Every error gives file:line:column, the offending form, and the fix as code:

```text
main.tsp:12:9: u8 + u16: types must match; write (+ (as u16 a) b)
main.tsp:4:1: this ( is never closed; its form starts here
main.tsp:20:5: let declares a variable here, it has no body; put the body after it
main.tsp:7:5: (make Sprite :y 1 :x 2) fields out of order; write (make Sprite :x 2 :y 1)
```

Spellings from before 2026-10-26 each say what to write now: `(args ...)`
gives "parameters are always a list now: write (fn f () void ...)", `(u16 y)`
"write (as u16 y)", `(Sprite :x 1)` "write (make Sprite :x 1)", `(. (at sprites
i) x)` "write (at sprites i x)", and a `let`, `const` or `for` without a type
gets the type the checker found.

`tisple build --asm` prints each function's generated 6502 code under its
source line; `tisple expand` prints the program after macro expansion.

## Placement

The linker checks them and fails the build if one can't hold.

| Option | Meaning |
| --- | --- |
| `:address 0x6000` | start at exactly this address |
| `:align 256` | start on a multiple of 256 |
| `:boundary 256` | whole object inside one 256-byte block; for a split table, each byte table |
| `:in aux` | a `let` in aux memory, reached only through `lib/auxmem` (2026-10-27) |
| `:reserve` | with `:address` and no value: memory the program reaches by address, kept out of the layout (2026-10-27) |

Placement goes on top-level `let`s and `const` tables, after the value; `:in`
and `:reserve` on `let`s only. An explicit `:address` may point anywhere, a
hi-res page included (`(let screen (array u8 8192) :address 0x2000)`);
automatic placement never uses zero page, the stack, text page 1 or `$C000`
and up, and fills the rest by the memory map below.

A global with `:address` and no value is left untouched at startup, like
screen memory: it is not part of the binary and the startup never writes it,
in the backend and the interpreter alike (owner reading, 2026-10-26). It holds
whatever memory held (zero in a fresh interpreter or simulator), unless it lies
inside the binary's own bytes, which loading the binary writes. Every other
top-level `let` without a value starts at zero.

### Memory map

Implemented 2026-10-27, for programs bigger than one stretch of memory. A
program that fits from its origin is laid out exactly as before, byte for
byte (checked 2026-10-29 for every program and example), with one change for
an origin below `$4000`: hi-res page 1 is no longer kept out by itself, so a
program that draws there says so with `:reserve` (below). Past that, in this
order:

1. From the origin (`$6000`) up to the limit: `$95FF` under DOS (the zero-page
   save buffer is its last page), `$BEFF` for a self-booting disk.
2. Below the origin, from `$0800` (`$0B00` on a disk, past the loader), around
   text page 1 and every `:address` and `:reserve` let. Whole functions move
   there when the code does not fit above the origin, so a branch never leaves
   its function; data fills what is left.
3. The language card's bank 2, `$D000`-`$FFFF` (12 KB), when nothing needs the
   ROM there: no `jsr`, `io_read`, `io_write` or `(as (ptr t) n)` with a
   constant address from `$D000`, and no `:address` let reaching `$D000`. The
   startup switches its RAM in (`BIT $C083` twice) and the ROM back before it
   returns (`BIT $C082`). A computed `jsr` or pointer is not checked: it is
   taken to reach slot firmware (as the mouse library's `jsr` does) or the
   program's own memory, never the ROM. Bank 1's 4 KB is not used.
4. Lets `:in aux`: aux memory `$0800`-`$BFFF` (46 KB), apart from all of the
   above; an `:address` there must lie in `$0200`-`$BFFF`.

Each let is placed whole, first fit in declaration order: one that does not fit
what is left of a region goes on to the next, and a later, smaller one may
still fill the gap. programs/text_demo leaves about 11 KB of `$6000`-`$BEFF`
empty that way, since its next chapter is bigger.

Size limits (2026-10-29), less any `:reserve` let (8 KB a hi-res page) and
what the program's frames take:

| Build | Main memory | Language card | Aux memory | In all |
| --- | --- | --- | --- | --- |
| `tisple build -o`, a DOS 3.3 BRUN file | `$6000`-`$94FF` from the default origin, 13.25 KB, one stretch | no | no | 13.25 KB |
| `tisple run` | `$0800`-`$94FF` less text page 1, 35.25 KB | 12 KB | 46 KB | 93 KB |
| `tisple build --disk`, self-booting | `$0B00`-`$BEFF`, 45 KB | 12 KB | 46 KB | 103 KB |

The page after main memory's last byte (`$9500` or `$BF00`) holds the saved
zero page. A disk holds at most 10 segments in 528 pages, each segment
rounded up to whole tracks (docs/disks.md): a program within the table fits,
unless `:address` lets cut its memory into more than 10 pieces.

Each piece outside the first is a segment. `tisple run` loads them all and a
disk built with `--disk` boots them all (docs/disks.md), but one DOS 3.3 BRUN
file holds one stretch of main memory, so `tisple build -o` refuses a program
with segments and says to build a disk.

The screens. The hi-res pages and text page 2 are ordinary memory unless the
program keeps them, which takes one `let`:

```scheme
(let page1 (array u8 8192) :address 0x2000 :reserve)   ; lib/hgr has this one
(let dhgr_aux (array u8 8192) :address 0x2000 :in aux :reserve)
```

A `:reserve` let is not a variable: naming it in code is an error that says to
use a pointer, and a constant `(as (ptr u8) 0x2000)` into it is fine, since for
pointer provenance it is memory the program does not declare. It needs
`:address` and has no value. Why a `let` option and not a top-level form
listing screens: it is the one fact the linker needs (these bytes are taken),
it fits the declare shape and the placement slot that already exist, and it
works for any memory, a screen, a DOS buffer or a ROM routine's scratch,
without a vocabulary of screen names in the compiler. A plain `:address` let
with no value already kept its bytes; `:reserve` adds only that code reaches
them by address. A program that fits from its origin never reaches the screens,
so the rule matters once a program spills below `$6000`.

Aux data. `(let doc (array u8 30000) (make ...) :in aux)` is the whole language
change: code names `doc` only as the root of an `addr_of`, and an `at`, `set`
or plain use is an error that says to copy it with `lib/auxmem`:

```scheme
(auxmem.read (addr_of (at line 0)) (addr_of (at doc offset)) 80)  ; aux to main
(auxmem.write (addr_of (at doc offset)) (addr_of (at line 0)) 80) ; main to aux
```

The pointer `addr_of` gives holds an aux address; `load` through it would read
main memory, so pass it only to `lib/auxmem`. Aux lets never go in zero page,
and the interpreter does not model aux memory (its `jsr` needs the machine).

## Strings

A string literal in code, `"HELLO"`, is a `(ptr u8)` to its bytes and a 0
after them (2026-10-29). The bytes are plain ASCII, 0 to 127; libraries apply
the screen encoding. Each different text is one const table, shared by every
use and placed with the other const tables; `(const GREETING (ptr u8)
"HELLO")` names one, and `GREETING` reads like the literal. The escapes are the
reader's, `\n \t \r \0 \\ \" \xHH`: `\r` is RETURN, which lib/text40 and
lib/text80 take as a new line, and a `\0` ends the string early. Nothing writes
a string; a store through one is undefined. A string cannot start a top-level
`let` or fill a const table, since its address is known only once the program
is laid out.

A const table, a string's or any other, that no kept function reads is left
out of the binary.

## Exact delays

`(wait_cycles n)` takes exactly n cycles wherever the code lands (owner,
2026-10-29; src/asm/wait.hpp).

- A constant n is 2 to 65535; 0 and 1 are errors that say what to write, since
  no 6502 instruction takes 1 cycle. A constant is whatever the checker folds:
  literals, consts and operators on them. The code is NOPs, `BIT $00` and
  `PHP`/`PLP` pairs around a counted loop when that is shorter (A counts, kept
  with `PHA`/`PLA`, and X too for long waits): the fewest bytes, at most 11 up
  to 1300 cycles and 23 up to 65535. A taken branch costs a cycle more when it
  crosses a page, so the loop's counts are worked out for the address it lands
  at, in a size that is the same at every address.
- A variable `u16` n takes exactly n cycles too, counted from the start of the
  wait, so a constant n and the same n in a variable wait alike, whatever the
  optimiser folds. Its own fixed cost is 69 cycles, so an n below 69 takes 69:
  that least is `timing.WAIT_MIN` in lib/timing. The wait reads n itself, with
  absolute loads wherever n lives, takes off the 69, then counts 256-cycle
  rounds for the high byte and takes the low byte bit by bit; a `JMP` over a
  filler keeps every branch inside its page (76 bytes). When n is more than a
  variable or field (a sum, an element at a computed index), the code working
  it out into a temporary comes first and is not part of the wait.
- Either keeps A, X, Y and memory (a variable wait counts in the compiler's
  zero-page scratch) and may change N, V, Z and C. It is an effect like
  `io_read`: never dropped, merged or moved. The interpreter does nothing for it.

This is how a program asks for timing. The optimiser may delete an effect-free
loop when the loop provably terminates (every `for`; a `while` whose counter moves
monotonically to its bound); an effect-free loop that never terminates is kept,
since removing it would change behaviour. Frame-level waits stay in the library
(`timing.frame`, `timing.frames`).

## Planned after the first backend (owner, 2026-10-26)

- Tail calls: a call in tail position compiles to `JMP`, not `JSR` + `RTS`.
  Backend only, no syntax.

Ideas, in order of cost, taken up when real programs need them:

1. Tail recursion as loops: a self-call in tail position jumps to the top, no
   frame. Comes free with tail calls, so the no-recursion rule can relax for it.
2. Recursion at compile time: done with computing macros. A recursive macro
   runs while compiling, so `(fib 10)` with `fib` a macro becomes `55`; typed
   functions called at compile time still follow the no-recursion rule.
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

General recursion, inline assembly. Both planned; none needed to prove the backend. `jsr` (Machine code) calls code
that already exists; it is not an inline assembler.
