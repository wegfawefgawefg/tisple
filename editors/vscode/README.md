# Tisple ][ for VS Code

Syntax highlighting, `;` comments and parenthesis matching for `.tsp` files.
Parentheses get VS Code's own bracket-pair colors.

`syntaxes/tisple.tmLanguage.json` is generated from the keyword lists in
`src/reader/keywords.cpp`, the same lists `tisple show` uses. After changing
them, run `tisple gen-editors editors`; a test fails while the file is stale.

Install by linking this folder into the extensions folder, then reload VS Code:

```sh
ln -s "$PWD/editors/vscode" ~/.vscode/extensions/tisple.tisple-syntax-0.1.0
```

Clash to know about: Microsoft's TypeSpec also uses `.tsp`. If the TypeSpec
extension is ever installed, it may claim `.tsp` files. Pick Tisple per file
with "Change Language Mode", or for good in settings:

```json
"files.associations": { "*.tsp": "tisple" }
```
