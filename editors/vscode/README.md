# Tisple ][ for VS Code

- Highlighting, `;` comments, parenthesis matching and bracket-pair colors for `.tsp`.
- Errors from `tisple check` on open and save, shown inline.
- Format on save through `tisple fmt`.
- **Tisple: Run on Apple II** (F5, or the play button): builds the open program
  and runs it in the built-in Apple IIe window.

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
