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

(fn spawn (args (x u8) (y u8))
    (if (= sprite_count MAX_SPRITES)
        (return false))
    (set (at sprites sprite_count) (Sprite x y true))
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

## Keywords

`:name` marks a declaration option, and that is all it does: `:address`,
`:align`, `:boundary` (and later `:in`) on `let`. Not values, not named
arguments, not struct fields. Struct literals are positional:
`(Sprite 10 20 true)`.

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
  widening zero-extends from unsigned and sign-extends from signed. Pointers
  convert only with `as`, so `(ptr ...)` is always a type.
- A literal has no type of its own. It takes the type its context needs and is
  an error if out of range (`(+ x 300)` with `x : u8` fails). With no context
  (`(let n 5)`) it is `u8` if it fits, else `u16`. Negative literals default to
  `i8`/`i16`.
- Arithmetic wraps: `(+ 255 1)` at `u8` is `0`. Comparisons are signed for
  `i8`/`i16`, unsigned otherwise.
- `*` exists at every width and compiles to a routine; `/` and `%` likewise,
  later.
- Bit operators are symbols, like arithmetic: `(& a b)`, `(| a b)`, `(^ a b)`,
  `(~ a)`, `(<< x n)`, `(>> x n)`. `>>` is logical for unsigned, arithmetic for
  signed.

## Forms

| Form | Meaning |
| --- | --- |
| `(const name type value)` | compile-time constant; `type` optional |
| `(let name type value)` | variable; `type` or `value` may be omitted, not both |
| `(set place value)` | assign to a name, `(at ...)` or `(. ...)` |
| `(fn name (args (p t) ...) body ...)` | function; `args` omitted when none |
| `(if c then else)` | both arms one form; with `else` it has a value, without it is a statement |
| `(do body ...)` | group forms; value is the last |
| `(while c body ...)` | loop |
| `(for i type start end body ...)` | `i` from `start` up to, not including, `end`; `type` optional, so `(for i u8 0 256 ...)` covers every byte |
| `(match x (pattern form) ...)` | first matching clause; patterns are literals, enum members, or `_` |
| `(return value)` / `(return)` / `(break)` / `(continue)` | early exits |
| `(and a b)` `(or a b)` `(not a)` | short-circuit, on `bool` |
| `(at arr i)` | array element; `i : u8` when the array has at most 256 elements |
| `(. s name ...)` | struct field; chains, so `(. s pos x)` is `s.pos.x` |
| `(load p)` / `(store p v)` | read / write through a pointer; the only way to do either |
| `(addr place)` | address of a variable, element or field |
| `(as (ptr t) n)` | the `u16` address `n` as a `(ptr t)`, and back with `(as u16 p)` |
| `(io_read t addr)` / `(io_write addr v)` | hardware access, `t` is `u8` or `u16` (low byte first); never dropped, merged or reordered |

A function's value is its last form, unless it ends in a statement, in which
case it returns nothing. Return type is inferred from the last form and every
`return`. Calls always use parentheses, `(read_key)`; only the declaration
omits an empty `args`.

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
  Macros that compute at compile time can come later by running the reference
  interpreter on macro bodies.
- `tisple expand file.tsp` prints the expanded program through the formatter.

## Memory and calls (first backend)

- Globals get fixed addresses from the linker. The interpreter uses the same
  layout, so pointer programs agree byte for byte.
- No recursion in this subset: the checker rejects call-graph cycles. Every
  function gets a static frame; the backend may overlap frames that can't be
  live at the same time.
- Return value: `u8`/`i8`/`bool` in A, 16-bit in A (low) and X (high).
- Arrays of structs are stored one array per field unless something takes an
  element's address; then they are packed.
- `(ptr t)` arithmetic: `(+ p n)` moves by `n` elements, not bytes (settled).

## Diagnostics

Every error gives file:line:column, the offending form, and the fix as code:

```text
main.tsp:12:9: u8 + u16: types must match; write (+ (u16 a) b)
main.tsp:4:1: this ( is never closed; its form starts here
main.tsp:20:5: let declares a variable here, it has no body; put the body after it
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
| `:boundary 256` | whole object inside one 256-byte block |

## Out of the first subset

Named regions, strings, function pointers, recursion, inline assembly,
`/` and `%`. Macro expansion comes right after the first backend. All planned; none needed to prove the backend.

## Open questions for the owner

1. Settled 2026-10-26: field access is `(. s x)`.
2. Settled 2026-10-26: `if` + `do`, no `when`/`unless`; `cond` only if else-if chains get ugly.
3. Settled 2026-10-26: pointer arithmetic moves by elements; `(ptr u8)` for byte offsets.
