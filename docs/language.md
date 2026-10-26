# Tisple ][ language: first subset

Started: 2026-10-26. Everything here is *proposed* until the owner settles it.
This is the subset the checker, reference interpreter and first backend build
against. Grow it when real programs need more.

## Program

A program is one or more `.tsp` files of top-level forms: `const`, `let`,
`struct`, `fn`. Order does not matter at top level. `main` is the entry point.

```scheme
(const max_sprites u8 32)

(struct sprite (x u8) (y u8) (active bool))

(let sprites (array sprite max_sprites))
(let sprite_count u8 0)

(fn spawn (args (x u8) (y u8))
    (when (= sprite_count max_sprites)
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
| `(set place value)` | assign to a name, `(at ...)`, `(field ...)` or `(load ...)` |
| `(fn name (args (p t) ...) body ...)` | function; `args` omitted when none |
| `(if c then else)` | expression; both arms one form, same type |
| `(when c body ...)` / `(unless c body ...)` | statement, no value |
| `(do body ...)` | group forms; value is the last |
| `(while c body ...)` | loop |
| `(for i start end body ...)` | `i` from `start` up to, not including, `end` |
| `(return value)` / `(break)` / `(continue)` | early exits |
| `(and a b)` `(or a b)` `(not a)` | short-circuit, on `bool` |
| `(at arr i)` | array element; `i : u8` when the array has at most 256 elements |
| `(field s name)` | struct field |
| `(load p)` / `(store p v)` | through a pointer |
| `(addr place)` | address of a variable, element or field |
| `(io_read t addr)` / `(io_write addr v)` | hardware access, never dropped, merged or reordered |

A function's value is its last form, unless it ends in a statement, in which
case it returns nothing. Return type is inferred from the last form and every
`return`. A bare name of a zero-argument function is a call.

Locals declared with `let` inside a body live to the end of the enclosing body.
No shadowing in the same body.

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
macros, modules, `/` and `%`. All planned; none needed to prove the backend.

## Open questions for the owner

1. `(field s x)` is long for the most common operation. A shorter spelling,
   e.g. `s.x` read as one symbol, would read better.
2. `if` with single-form arms plus `do`, versus `cond` or `when`/`unless` only.
3. Pointer arithmetic by elements (C-like) or by bytes.
