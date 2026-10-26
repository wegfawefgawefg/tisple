// What the checker knows, in the editor: semantic tokens, hover, go to definition, find
// references, the outline and live diagnostics, all from `tisple index` (index_client.js).

const vscode = require("vscode");
const fs = require("fs");
const path = require("path");
const ix = require("./index_client");

// standard token types and modifiers, so every theme colours them
const tokenTypes = ["namespace", "type", "struct", "enum", "parameter", "variable", "property",
    "enumMember", "function", "macro", "keyword", "operator"];
const tokenModifiers = ["declaration", "readonly", "defaultLibrary"];
const legend = new vscode.SemanticTokensLegend(tokenTypes, tokenModifiers);

// index kind -> [token type, modifiers]
const kindStyles = {
    "function": ["function", []],
    "macro": ["macro", []],
    "parameter": ["parameter", []],
    "local": ["variable", []],
    "global": ["variable", []],
    "constant": ["variable", ["readonly"]],
    "struct": ["struct", []],
    "enum": ["enum", []],
    "enum member": ["enumMember", ["readonly"]],
    "field": ["property", []],
    "module": ["namespace", []],
    "built-in": ["function", ["defaultLibrary"]],
    "special form": ["keyword", []],
    "primitive type": ["type", ["defaultLibrary"]],
};

function tokenStyle(index, token) {
    const style = kindStyles[token.kind];
    if (!style) {
        return null;
    }
    let type = style[0];
    const def = ix.definitionOf(index, token);
    // + - < << and friends read better as operators than as functions
    if (token.kind === "built-in" && def && !/^[a-z_]/.test(def.name)) {
        type = "operator";
    }
    let bits = 0;
    const mods = style[1].slice();
    if (token.declaration) {
        mods.push("declaration");
    }
    if (def && def.library) {
        mods.push("defaultLibrary");
    }
    for (const mod of mods) {
        bits |= 1 << tokenModifiers.indexOf(mod);
    }
    return [tokenTypes.indexOf(type), bits];
}

async function provideSemanticTokens(doc) {
    const index = await ix.indexDocument(doc);
    const builder = new vscode.SemanticTokensBuilder(legend);
    if (!index) {
        return builder.build();
    }
    const tokens = (index.tokensByFile.get(path.resolve(doc.fileName)) || []).slice();
    tokens.sort((a, b) => a.line - b.line || a.column - b.column);
    for (const token of tokens) {
        const style = tokenStyle(index, token);
        if (style) {
            builder.push(token.line - 1, token.column - 1, token.length, style[0], style[1]);
        }
    }
    return builder.build();
}

async function provideHover(doc, position) {
    const index = await ix.indexDocument(doc);
    const token = index && ix.tokenAt(index, doc, position);
    const def = index && ix.definitionOf(index, token);
    if (!def) {
        return null;
    }
    const text = new vscode.MarkdownString();
    text.appendCodeblock(def.type, "tisple");
    let where = def.qualified !== def.name ? ` \`${def.qualified}\`` : "";
    if (def.file >= 0 && token.kind !== "module") {
        where += ` in ${path.basename(index.files[def.file])}`;
    }
    text.appendMarkdown(`*${token.kind}*${where}`);
    if (def.doc) {
        text.appendMarkdown("\n\n");
        text.appendText(def.doc);
    }
    return new vscode.Hover(text, ix.tokenRange(token));
}

async function provideDefinition(doc, position) {
    const index = await ix.indexDocument(doc);
    const def = index && ix.definitionOf(index, ix.tokenAt(index, doc, position));
    return def ? ix.definitionLocation(index, def) : null;
}

// the program's entry for a module: the nearest main.tsp in a folder above it
function programEntry(fileName) {
    let dir = path.dirname(fileName);
    const folder = vscode.workspace.getWorkspaceFolder(vscode.Uri.file(fileName));
    const stop = folder ? folder.uri.fsPath : path.parse(dir).root;
    for (let i = 0; i < 6; i++) {
        const candidate = path.join(dir, "main.tsp");
        if (candidate !== path.resolve(fileName) && fs.existsSync(candidate)) {
            return candidate;
        }
        if (dir === stop || path.dirname(dir) === dir) {
            break;
        }
        dir = path.dirname(dir);
    }
    return null;
}

async function provideReferences(doc, position, context) {
    const index = await ix.indexDocument(doc);
    const def = index && ix.definitionOf(index, ix.tokenAt(index, doc, position));
    if (!def) {
        return null;
    }
    const key = ix.definitionKey(index, def);
    // this file's program, the program its nearest main.tsp makes, and every open file's
    const indexes = [index];
    const entry = programEntry(doc.fileName);
    if (entry) {
        indexes.push(await ix.indexFile(entry));
    }
    indexes.push(...(await ix.cachedIndexes()));
    const seen = new Set();
    const locations = [];
    for (const other of indexes) {
        if (!other) {
            continue;
        }
        for (const token of other.tokens) {
            const tokenDef = ix.definitionOf(other, token);
            if (!tokenDef || ix.definitionKey(other, tokenDef) !== key) {
                continue;
            }
            if (token.declaration && !context.includeDeclaration) {
                continue;
            }
            const file = other.files[token.file];
            const spot = `${file}:${token.line}:${token.column}`;
            if (!seen.has(spot)) {
                seen.add(spot);
                locations.push(new vscode.Location(vscode.Uri.file(file), ix.tokenRange(token)));
            }
        }
    }
    return locations;
}

const symbolKinds = {
    "function": vscode.SymbolKind.Function,
    "macro": vscode.SymbolKind.Function,
    "constant": vscode.SymbolKind.Constant,
    "global": vscode.SymbolKind.Variable,
    "struct": vscode.SymbolKind.Struct,
    "enum": vscode.SymbolKind.Enum,
    "field": vscode.SymbolKind.Field,
    "enum member": vscode.SymbolKind.EnumMember,
    "module": vscode.SymbolKind.Module,
};

function documentSymbol(index, symbol) {
    const def = symbol.def >= 0 ? index.definitions[symbol.def] : null;
    const range = ix.rangeOf(symbol.range);
    let selection = ix.rangeOf(symbol.selection);
    if (!range.contains(selection)) {
        selection = range;
    }
    const result = new vscode.DocumentSymbol(symbol.name, def ? def.type : "",
        symbolKinds[symbol.kind] || vscode.SymbolKind.Object, range, selection);
    result.children = symbol.children.map((child) => documentSymbol(index, child));
    return result;
}

async function provideDocumentSymbols(doc) {
    const index = await ix.indexDocument(doc);
    if (!index) {
        return [];
    }
    const file = path.resolve(doc.fileName);
    return index.symbols.filter((s) => index.files[s.file] === file)
        .map((s) => documentSymbol(index, s));
}

// ---- diagnostics: from the same index run, debounced while typing

let diagnostics = null;
const timers = new Map();

// an error's range: one line, at least the word it starts at
function errorRange(doc, r) {
    const start = new vscode.Position(r.line - 1, r.column - 1);
    if (r.end_line === r.line && r.end_column > r.column) {
        return ix.rangeOf(r);
    }
    if (doc && start.line < doc.lineCount) {
        let at = start;
        if (doc.lineAt(start.line).text[start.character] === "(") {
            at = start.translate(0, 1);
        }
        const word = doc.getWordRangeAtPosition(at, /[^\s()'`,";]+/);
        if (word) {
            return new vscode.Range(start, word.end);
        }
    }
    return new vscode.Range(start, start.translate(0, 1));
}

function openDocument(file) {
    return vscode.workspace.textDocuments.find(
        (d) => d.uri.scheme === "file" && path.resolve(d.fileName) === file);
}

function publishDiagnostics(doc, index) {
    if (!index) {
        return;
    }
    const own = path.resolve(doc.fileName);
    const byFile = new Map(index.files.map((f) => [f, []]));
    for (const error of index.errors) {
        const file = index.files[error.file];
        if (!byFile.has(file)) {
            continue;
        }
        const message = error.macro ? `${error.message} (in macro ${error.macro})` : error.message;
        const d = new vscode.Diagnostic(errorRange(openDocument(file), error.range), message,
            vscode.DiagnosticSeverity.Error);
        d.source = "tisple";
        byFile.get(file).push(d);
    }
    for (const [file, list] of byFile) {
        // an open file's errors come from its own runs, which see its unsaved text
        if (file !== own && openDocument(file)) {
            continue;
        }
        diagnostics.set(vscode.Uri.file(file), list);
    }
}

function isTisple(doc) {
    return doc.languageId === "tisple" && doc.uri.scheme === "file";
}

function checkNow(doc) {
    if (!isTisple(doc)) {
        return;
    }
    const version = doc.version;
    ix.indexDocument(doc).then((index) => {
        if (doc.version === version) {
            publishDiagnostics(doc, index);
        }
    });
}

function checkSoon(doc) {
    if (!isTisple(doc)) {
        return;
    }
    const key = doc.uri.toString();
    clearTimeout(timers.get(key));
    timers.set(key, setTimeout(() => {
        timers.delete(key);
        checkNow(doc);
    }, 300));
}

function register(context) {
    const selector = { language: "tisple", scheme: "file" };
    diagnostics = vscode.languages.createDiagnosticCollection("tisple");
    context.subscriptions.push(diagnostics);
    context.subscriptions.push(vscode.languages.registerDocumentSemanticTokensProvider(selector,
        { provideDocumentSemanticTokens: provideSemanticTokens }, legend));
    context.subscriptions.push(vscode.languages.registerHoverProvider(selector,
        { provideHover }));
    context.subscriptions.push(vscode.languages.registerDefinitionProvider(selector,
        { provideDefinition }));
    context.subscriptions.push(vscode.languages.registerReferenceProvider(selector,
        { provideReferences }));
    context.subscriptions.push(vscode.languages.registerDocumentSymbolProvider(selector,
        { provideDocumentSymbols }));
    context.subscriptions.push(vscode.workspace.onDidOpenTextDocument(checkNow));
    context.subscriptions.push(vscode.workspace.onDidSaveTextDocument(checkNow));
    context.subscriptions.push(vscode.workspace.onDidChangeTextDocument((e) => checkSoon(e.document)));
    context.subscriptions.push(vscode.workspace.onDidCloseTextDocument((doc) => {
        ix.forget(doc);
    }));
    vscode.workspace.textDocuments.forEach(checkNow);
}

module.exports = { register };
