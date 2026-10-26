# Tisple ][ for VS Code

- Highlighting from the checker: every name is coloured by what it is (function, macro,
  parameter, local, global, constant, struct, enum, member, field, module, built-in),
  with declarations and `lib/` names marked. The TextMate grammar covers the rest.
- Hover: what a name is, its type or signature, and its declaration's comment.
- Go to definition and find references, across the files of the program.
- Outline of each file's top-level forms.
- Errors as you type (300 ms after the last key), from the unsaved buffer.
- Format on save through `tisple fmt`.
- **Tisple: Run on Apple II** (F5, or the play button): builds the open program
  and runs it in the built-in Apple IIe window.

Everything but formatting and running comes from one `tisple index file.tsp --stdin` run
per version of the buffer: load, macro expansion and the checker, no backend, a few ms.
Its JSON is described in `src/ide/index.hpp`. Layout errors (addresses that do not fit)
only show in `tisple check` and `tisple build`, since the index skips the layout.

It finds the compiler at `build-debug/tisple` in a folder above the file, or
`tisple` on PATH; set `tisple.path` to override.

`syntaxes/tisple.tmLanguage.json` is generated from the keyword lists in
`src/reader/keywords.cpp`, the same lists `tisple show` uses. After changing
them, run `tisple gen-editors editors`; a test fails while the file is stale.

Install (VS Code only loads extensions installed this way, not symlinks):

```sh
python3 editors/vscode/package_vsix.py /tmp/tisple.vsix
code --install-extension /tmp/tisple.vsix
```

Clash to know about: Microsoft's TypeSpec also uses `.tsp`. If the TypeSpec
extension is ever installed, it may claim `.tsp` files. Pick Tisple per file
with "Change Language Mode", or for good in settings:

```json
"files.associations": { "*.tsp": "tisple" }
```
