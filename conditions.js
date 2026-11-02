// Conditions over a running game's state, for the pad (pad.js): a key's "when" and "live".
//
// Conditions are C-like expressions over numbers:
//   state.phase == state.Phase.player && state.played[2] == 0
// with == != < <= > >= && || ! + - and ( ), a var with [index] for an element. A name is one
// of the config's "names" (named conditions, which may use each other), else a var or an enum
// member of the game's build: game.json "vars" and "enums", which publish.py takes from the
// build's DISK.vars.json, each var {at, bytes, high, count, step, signed, aux}.

const COND_TOKEN = /\s*(?:(0x[0-9a-fA-F]+|\d+)|([A-Za-z_][A-Za-z0-9_/]*(?:\.[A-Za-z_][A-Za-z0-9_/]*)*)|(==|!=|<=|>=|&&|\|\||[-+!<>()[\]]))/y;

function condTokens(text) {
    const out = [];
    COND_TOKEN.lastIndex = 0;
    while (COND_TOKEN.lastIndex < text.length) {
        if (/^\s*$/.test(text.slice(COND_TOKEN.lastIndex))) {
            break;
        }
        const at = COND_TOKEN.lastIndex;
        const m = COND_TOKEN.exec(text);
        if (!m) {
            throw new Error("cannot read '" + text.slice(at) + "' in: " + text);
        }
        if (m[1] !== undefined) {
            out.push({ num: Number(m[1]) });
        } else if (m[2] !== undefined) {
            out.push({ name: m[2] });
        } else {
            out.push({ op: m[3] });
        }
    }
    return out;
}

// one var's element as a number, index a compiled expression (or null for element 0)
function varRead(v, index) {
    return (read) => {
        const i = index ? index(read) : 0;
        if (i < 0 || i >= (v.count || 1)) {
            return 0;
        }
        const at = v.at + i * (v.step || 0);
        let x = read(at, v.aux ? 1 : 0);
        if (v.bytes === 2) {
            x |= read(at + v.high, v.aux ? 1 : 0) << 8;
        }
        if (v.signed) {
            x = v.bytes === 2 ? (x << 16) >> 16 : (x << 24) >> 24;
        }
        return x;
    };
}

// text compiled to a function of read(address, aux) giving a number; throws on a mistake
function compileCondition(text, game, named) {
    const tokens = condTokens(String(text));
    let pos = 0;
    const peekOp = (op) => pos < tokens.length && tokens[pos].op === op;
    const take = (op) => {
        if (!peekOp(op)) {
            throw new Error("wants " + op + " in: " + text);
        }
        pos++;
    };
    const binary = (next, ops) => () => {
        let a = next();
        while (pos < tokens.length && ops[tokens[pos].op]) {
            const f = ops[tokens[pos++].op];
            const l = a;
            const r = next();
            a = (read) => f(l, r, read);
        }
        return a;
    };
    let orExpr = null;
    const primary = () => {
        const t = tokens[pos++];
        if (!t) {
            throw new Error("ends too soon: " + text);
        }
        if (t.num !== undefined) {
            return () => t.num;
        }
        if (t.op === "(") {
            const e = orExpr();
            take(")");
            return e;
        }
        if (t.op === "!") {
            const e = primary();
            return (read) => (e(read) ? 0 : 1);
        }
        if (t.op === "-") {
            const e = primary();
            return (read) => -e(read);
        }
        if (t.name === undefined) {
            throw new Error("unexpected " + t.op + " in: " + text);
        }
        if (t.name === "true" || t.name === "false") {
            const n = t.name === "true" ? 1 : 0;
            return () => n;
        }
        if (game.names && game.names[t.name] !== undefined) {
            return named(t.name);
        }
        if (game.enums && game.enums[t.name] !== undefined) {
            const n = game.enums[t.name];
            return () => n;
        }
        const v = game.vars && game.vars[t.name];
        if (!v) {
            throw new Error("no var " + t.name);
        }
        let index = null;
        if (peekOp("[")) {
            pos++;
            index = orExpr();
            take("]");
        }
        return varRead(v, index);
    };
    const sum = binary(primary, { "+": (l, r, x) => l(x) + r(x), "-": (l, r, x) => l(x) - r(x) });
    const compare = binary(sum, {
        "==": (l, r, x) => (l(x) === r(x) ? 1 : 0), "!=": (l, r, x) => (l(x) !== r(x) ? 1 : 0),
        "<": (l, r, x) => (l(x) < r(x) ? 1 : 0), "<=": (l, r, x) => (l(x) <= r(x) ? 1 : 0),
        ">": (l, r, x) => (l(x) > r(x) ? 1 : 0), ">=": (l, r, x) => (l(x) >= r(x) ? 1 : 0),
    });
    const andExpr = binary(compare, { "&&": (l, r, x) => (l(x) && r(x) ? 1 : 0) });
    orExpr = binary(andExpr, { "||": (l, r, x) => (l(x) || r(x) ? 1 : 0) });
    const e = orExpr();
    if (pos !== tokens.length) {
        throw new Error("more than one expression in: " + text);
    }
    return e;
}

if (typeof module !== "undefined") {
    module.exports = { compileCondition, condTokens }; // node, for the tests (tests/web)
}
