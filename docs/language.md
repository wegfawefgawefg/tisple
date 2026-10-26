# Tisple ][ language: first subset

Started: 2026-10-26. Everything here is *proposed* until the owner settles it.
This is the subset the checker, reference interpreter and first backend build
against. Grow it when real programs need more.

## Program

A program is one or more `.tsp` files of top-level forms: `const`, `let`,
`struct`, `enum`, `fn`. Order does not matter at top level. `main` is the entry point.

```scheme
(const max_sprites u8 32)

(struct sprite (x u8) (y u8) (active bool))

(let sprites (array sprite max_sprites))
(let sprite_count u8 0)

(fn spawn (args (x u8) (y u8))
    (if (= sprite_count max_sprites)
        (return false))
    (set (at sprites sprite_count) (sprite (x x) (y y) (active true)))
    (set sprite_count (+ sprite_count 1))
    true)
```

## Types

| Type | Meaning |
| --- | --- |
| `u8` `i8` `u16` `i16` | integers; arithmetic wraps |
| `bool` | `true` / `false`; not an integer |
| `(ptr t)` | 16-bit address of a `t` |
| `(array t n)` | `n` elements of `t`; `n` a constant |
| struct name | a declared `struct` |
| enum name | a declared `enum`; stored as `u8` |

Struct names are `snake_case` like everything else (`sprite`, not `Sprite`).
Types and values live in separate namespaces, so `(let sprite sprite)` is legal
if unwise.

## Integers

- No implicit widening or promotion. Both sides of `+`, `=`, `<` and so on must
  have the same type. `(+ (u16 a) b)` widens `a` explicitly.
- Conversions: `(u8 x)`, `(i8 x)`, `(u16 x)`, `(i16 x)`. Narrowing truncates;
  widening zero-extends from unsigned and sign-extends from signed.
- A literal has no type of its own. It takes the type its context needs and is
  an error if out of range (`(+ x 300)` with `x : u8` fails). With no context
  (`(let n 5)`) it is `u8` if it fits, else `u16`. Negative literals default to
  `i8`/`i16`.
- Arithmetic wraps: `(+ 255 1)` at `u8` is `0`. Comparisons are signed for
  `i8`/`i16`, unsigned otherwise.
- `*` exists at every width and compiles to a routine; `/` and `%` likewise,
  later. Shifts: `(shl x n)`, `(shr x n)` (logical for unsigned, arithmetic
  for signed).

## Forms

| Form | Meaning |
| --- | --- |
| `(const name type value)` | compile-time constant; `type` optional |
| `(let name type value)` | variable; `type` or `value` may be omitted, not both |
| `(set place value)` | assign to a name, `(at ...)`, `(. ...)` or `(load ...)` |
| `(fn name (args (p t) ...) body ...)` | function; `args` omitted when none |
| `(if c then else)` | both arms one form; with `else` it has a value, without it is a statement |
| `(do body ...)` | group forms; value is the last |
| `(while c body ...)` | loop |
| `(for i start end body ...)` | `i` from `start` up to, not including, `end` |
| `(match x (pattern form) ...)` | first matching clause; patterns are literals, enum members, or `_` |
| `(return value)` / `(break)` / `(continue)` | early exits |
| `(and a b)` `(or a b)` `(not a)` | short-circuit, on `bool` |
| `(at arr i)` | array element; `i : u8` when the array has at most 256 elements |
| `(. s name ...)` | struct field; chains, so `(. s pos x)` is `s.pos.x` |
| `(load p)` / `(store p v)` | through a pointer |
| `(addr place)` | address of a variable, element or field |
| `(io_read t addr)` / `(io_write addr v)` | hardware access, never dropped, merged or reordered |

A function's value is its last form, unless it ends in a statement, in which
case it returns nothing. Return type is inferred from the last form and every
`return`. A bare name of a zero-argument function is a call.

Locals declared with `let` inside a body live to the end of the enclosing body.
No shadowing in the same body.

## Enums and match

```scheme
(enum action move_left move_right fire)

(fn handle (args (a action))
    (match a
        (move_left (step 255))
        (move_right (step 1))
        (fire (do
            (shoot)
            (play_click)))))
```

- Members number from 0 in order. `(enum action (fire 7) ...)` sets a value.
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
- `(ptr t)` arithmetic: `(+ p n)` moves by `n` elements, not bytes.

## Out of the first subset

Placement controls, strings, function pointers, recursion, inline assembly,
modules, `/` and `%`. Macro expansion comes right after the first backend. All planned; none needed to prove the backend.

## Open questions for the owner

1. Settled 2026-10-26: field access is `(. s x)`.
2. Settled 2026-10-26: `if` + `do`, no `when`/`unless`; `cond` only if else-if chains get ugly.
3. Pointer arithmetic: recommended by elements (C-like); `(ptr u8)` for byte offsets. Awaiting owner.
