# Editor support

Both grammars are generated from `src/reader/keywords.cpp`, the lists
`tisple show` colors with, so the editors and the viewer agree. After changing
the lists:

```sh
./build-debug/tisple gen-editors editors
```

`tisple_tests` fails while a checked-in grammar is stale.

What gets colored: special forms (`fn let if match . as macro import` ...),
types (`u8 ptr array` and any `PascalCase` name), built-ins and operators
(`store io_read + & <<` ...), constants (`true false` and `ALL_CAPS` names),
numbers, strings, `:keywords`, quote marks (`` ` , ,@ ' ``) and comments.

## VS Code

See `vscode/README.md`. Short version:

```sh
ln -s "$PWD/editors/vscode" ~/.vscode/extensions/tisple.tisple-syntax-0.1.0
```

## Neovim

`nvim/` is a plain runtime folder (`ftdetect/`, `ftplugin/`, `syntax/`). Load it
as a package, which needs no change to `init.lua`:

```sh
mkdir -p ~/.local/share/nvim/site/pack/tisple/start
ln -s "$PWD/editors/nvim" ~/.local/share/nvim/site/pack/tisple/start/tisple
```

Or, from a config you manage, `vim.opt.runtimepath:append("/path/to/tisple/editors/nvim")`.

Rainbow parentheses need a plugin (for example rainbow-delimiters.nvim); the
syntax file gives parentheses the `Delimiter` group.

`.tsp` is also TypeSpec's extension; `ftdetect/tisple.vim` uses
`set filetype=tisple` so Tisple wins.
