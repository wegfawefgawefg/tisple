// Runs `tisple index` and keeps the result: one run per document version serves every
// provider (tokens, hover, definition, references, outline, diagnostics). The JSON shape is
// documented in src/ide/index.hpp; lines and columns there are 1-based UTF-16.

const vscode = require("vscode");
const cp = require("child_process");
const fs = require("fs");
const path = require("path");

let warnedMissing = false;

// the tisple binary: the setting, else build-debug/tisple in a folder above the file, else PATH
function tisplePath(fileName) {
    const configured = vscode.workspace.getConfiguration("tisple").get("path");
    if (configured) {
        return configured;
    }
    let dir = path.dirname(fileName);
    for (let i = 0; i < 8; i++) {
        const candidate = path.join(dir, "build-debug", "tisple");
        if (fs.existsSync(candidate)) {
            return candidate;
        }
        const parent = path.dirname(dir);
        if (parent === dir) {
            break;
        }
        dir = parent;
    }
    return "tisple";
}

// adds lookups the providers need: tokens by file
function prepare(index) {
    index.files = index.files.map((f) => path.resolve(f));
    index.tokensByFile = new Map();
    for (const token of index.tokens) {
        const file = index.files[token.file];
        if (!index.tokensByFile.has(file)) {
            index.tokensByFile.set(file, []);
        }
        index.tokensByFile.get(file).push(token);
    }
    return index;
}

// `tisple index file [--stdin]`; resolves to the prepared index, or null when it failed
function runIndex(fileName, text, onChild) {
    return new Promise((resolve) => {
        const tool = tisplePath(fileName);
        const args = text === null ? ["index", fileName] : ["index", fileName, "--stdin"];
        const options = { cwd: path.dirname(fileName), maxBuffer: 64 * 1024 * 1024 };
        const child = cp.execFile(tool, args, options, (error, stdout) => {
            if (error) {
                if (error.code === "ENOENT" && !warnedMissing) {
                    warnedMissing = true;
                    vscode.window.showWarningMessage(
                        "Tisple: no tisple binary found; set tisple.path or build the repo.");
                }
                resolve(null);
                return;
            }
            try {
                resolve(prepare(JSON.parse(stdout)));
            } catch (e) {
                resolve(null);
            }
        });
        if (text !== null) {
            child.stdin.on("error", () => {});
            child.stdin.end(text);
        }
        if (onChild) {
            onChild(child);
        }
    });
}

// uri -> { version, promise, child }: the latest version of each open document
const cache = new Map();

function indexDocument(doc) {
    const key = doc.uri.toString();
    const known = cache.get(key);
    if (known && known.version === doc.version) {
        return known.promise;
    }
    if (known && known.child) {
        known.child.kill(); // an older version nobody needs any more
    }
    const entry = { version: doc.version, child: null, promise: null };
    entry.promise = runIndex(doc.fileName, doc.getText(), (child) => {
        entry.child = child;
    }).then((index) => {
        entry.child = null;
        return index;
    });
    cache.set(key, entry);
    return entry.promise;
}

function forget(doc) {
    cache.delete(doc.uri.toString());
}

// a file's index from its open buffer if it has one, else from disk
function indexFile(fileName) {
    const resolved = path.resolve(fileName);
    const open = vscode.workspace.textDocuments.find(
        (d) => d.uri.scheme === "file" && path.resolve(d.fileName) === resolved);
    return open ? indexDocument(open) : runIndex(resolved, null, null);
}

// the indexes already made for open documents, for references across files
function cachedIndexes() {
    return Promise.all([...cache.values()].map((entry) => entry.promise));
}

function rangeOf(r) {
    return new vscode.Range(r.line - 1, r.column - 1, r.end_line - 1, r.end_column - 1);
}

function tokenRange(token) {
    return new vscode.Range(token.line - 1, token.column - 1, token.line - 1,
        token.column - 1 + token.length);
}

// the token under position in the document, or null
function tokenAt(index, doc, position) {
    const tokens = index.tokensByFile.get(path.resolve(doc.fileName)) || [];
    const line = position.line + 1;
    const column = position.character + 1;
    for (const token of tokens) {
        if (token.line === line && token.column <= column && column <= token.column + token.length) {
            return token;
        }
    }
    return null;
}

function definitionOf(index, token) {
    return token && token.def >= 0 ? index.definitions[token.def] : null;
}

// the same definition seen from two index runs: same file, place and name
function definitionKey(index, def) {
    if (def.file < 0) {
        return `builtin:${def.name}`;
    }
    return `${index.files[def.file]}:${def.range.line}:${def.range.column}:${def.name}`;
}

function definitionLocation(index, def) {
    if (def.file < 0) {
        return null;
    }
    return new vscode.Location(vscode.Uri.file(index.files[def.file]), rangeOf(def.range));
}

module.exports = {
    tisplePath,
    indexDocument,
    indexFile,
    forget,
    cachedIndexes,
    rangeOf,
    tokenRange,
    tokenAt,
    definitionOf,
    definitionKey,
    definitionLocation,
};
