# Tisple ][

Typed Lisp for the Apple II with its own optimizing 6502 backend. Read
`docs/design.md` first: why the language exists, syntax, milestones.

- Build: `cmake --preset dev && cmake --build --preset dev` (Ninja, ccache, lld).
  Tests: `./build-debug/tisple_tests`. Keep the build fast; no heavy deps.
- C++20 in the splonks-cpp style: `namespace tisple`, PascalCase functions,
  snake_case fields and locals, `kConstants`, plain structs, no exceptions in
  compiler code. Format with the repo `.clang-format`.
- One `tisple` binary with subcommands in `src/main.cpp`; all other code lives
  in `tisple_core` so tests link the same code.
- Tests: add a `RunXTests(TestResults&)` per area and one call in
  `tests/test_main.cpp`. Big downloaded test data goes in `tests/data/`
  (gitignored), fetched by a script in `scripts/`.
- Source files use `.tsp`. Name in prose is "Tisple ][", everywhere else `tisple`.

## File Size

- Target file size is roughly 300-500 lines.
- If a file is clearly growing past that range, prefer splitting it.
- Split by cohesive responsibility, not arbitrarily.
- Small exceptions are fine when the language or framework strongly favors a
  single file, or when a split would make navigation worse.

## Code Organization

- Group code by functionality.
- Keep related functions near each other.
- Prefer files that have one clear job.
- Prefer mostly flat module structure.
- Avoid deep module nesting unless there is a strong, concrete reason for it.
- Small groups are fine where they clearly improve ownership or navigation.
- Prefer modules organized around behavior and ownership, not around vague
  categories like `utils` unless the helpers are truly shared and generic.
- In backend code, keep request parsing, validation, business logic, and data
  access reasonably separated unless the code is trivial.

## Style

- Default style target is "C+" / "C+-" code:
  - direct
  - explicit
  - readable
  - low-magic
  - modest abstraction
  - easy to trace in a debugger
- Prefer straightforward control flow over clever indirection.
- Avoid deep nesting in `if` statements, `match` expressions, and general logic.
- Prefer early returns, helper functions, and flatter branching when that keeps
  behavior clearer.
- Prefer obvious data movement over framework tricks.
- Prefer small helper functions over deep abstraction stacks.
- Avoid unnecessary genericization.
- Avoid premature reuse that makes local behavior harder to understand.
- Keep naming concrete and descriptive.

## Language Pragmatism

- Do not force C-like structure where it clearly fights the language.
- Follow the grain of the language when the idiomatic form is materially better.
- In Rust, this means using enums, pattern matching, `Result`, and normal
  ownership-based design instead of awkward pseudo-C emulation.
- The rule is: prefer explicitness first, but do not write unnatural code just
  to mimic C.
- In c++ though, you'll mostly be making things c-like.

## Change Discipline

- Preserve existing behavior unless the task is to change it.
- Do not perform broad style rewrites unless requested.
- Do not move code across many files just to satisfy a size guideline if the
  current structure is still easier to work with.
- When splitting files, keep the resulting layout obvious and boring.

## When in Doubt

- Choose the more explicit option.
- Choose the easier-to-debug option.
- Choose the layout that keeps related functionality together.
- Choose the solution with fewer hidden config dependencies.
