// The player's controls and layout, drawn in the Apple II's own letters (font.js, from the
// machine's font): the bar, the game's buttons (game.json "pad", or the older "buttons"),
// the abc keyboard, and where the screen goes. Portrait: the screen on top, the buttons under
// it, the most used ones biggest and lowest, in thumb reach. Landscape and desktop: the screen
// as large as fits; a game whose screen takes taps (game.json "taps", or "mouse") gets no
// buttons there, any other has them in a column beside it. player.js owns the machine and calls press and
// release, which it gives to padSetup.
//
// "pad" is a list of rows, top to bottom:
//   {"size": "small" | "medium" | "big", "keys": [{"label": "END TURN", "key": "E",
//    "look": "card" | "white" | "violet" | "orange" | "green", "span": 2, "hint": "E",
//    "sub": "smaller words under the label"}]}
// A key is a character, a number, or RETURN ESC SPACE LEFT RIGHT UP DOWN TAB DELETE; the hint
// (the key letter, by default) shows small in a corner. Small rows sit at the right, out of
// the way. A key may also have "when" (it shows only while that holds) and "live" (while it
// shows, it can be pressed only while that holds; else it is dim): conditions over the game's
// state (padFollow, below), and "state_only": true, a key shown only when the state is read.

"use strict";

const PAD_KEY_NAMES = { RETURN: 13, ESC: 27, SPACE: 32, LEFT: 8, RIGHT: 21, UP: 11, DOWN: 10,
                        TAB: 9, DELETE: 127 };
const PAD_ENTITIES = { "&larr;": "←", "&rarr;": "→", "&uarr;": "↑",
                       "&darr;": "↓" };

const pad = {
    press: null,   // player.js: press(code), release(code)
    release: null,
    game: null,
    labels: [],    // every label element, redrawn when the device pixel ratio changes
    keys: [],      // the game's buttons: {el, spec, row, up, when, live}, when and live compiled
    rows: [],      // {el, size, keys}
    follows: false, // the conditions compiled: the pad can follow the game's state
    seen: "",      // the looks the state gave last time, and the ones shown (padFollow)
    shown: null,
};

function padKeyCode(spec) {
    if (typeof spec === "number") {
        return spec;
    }
    if (PAD_KEY_NAMES[spec] !== undefined) {
        return PAD_KEY_NAMES[spec];
    }
    return spec.toUpperCase().charCodeAt(0);
}

// a key's code, or the joystick: "stick": "left" (and right, up, down) holds paddle 0 or 1 at
// its end while pressed, 0x100 + 0-3; "button": 0-2 holds that button, 0x200 + n
const STICK_WAYS = ["left", "right", "up", "down"];

function padCode(spec) {
    if (STICK_WAYS.indexOf(spec.stick) >= 0) {
        return 0x100 + STICK_WAYS.indexOf(spec.stick);
    }
    if (spec.button === 0 || spec.button === 1 || spec.button === 2) {
        return 0x200 + spec.button;
    }
    return padKeyCode(spec.key);
}

// ---- letters: text in the machine's 5x7 dots, as a mask the element's colour shows through

// the text drawn at dot pixels a dot (CSS px), whole device pixels so the dots stay square
function labelMask(text, dot) {
    const ratio = window.devicePixelRatio || 1;
    const d = Math.max(1, Math.round(dot * ratio));
    const canvas = document.createElement("canvas");
    canvas.width = Math.max(1, (text.length * 6 - 1) * d);
    canvas.height = 7 * d;
    const g = canvas.getContext("2d");
    g.fillStyle = "#fff";
    for (let i = 0; i < text.length; i++) {
        const rows = APPLE_FONT[text[i]] || APPLE_FONT["?"];
        for (let r = 0; r < 7; r++) {
            for (let c = 0; c < 5; c++) {
                if (rows[r] & (16 >> c)) {
                    g.fillRect((i * 6 + c) * d, r * d, d, d);
                }
            }
        }
    }
    return { url: canvas.toDataURL(), width: canvas.width / ratio, height: canvas.height / ratio };
}

function drawLabel(el) {
    const m = labelMask(el.dataset.text, Number(el.dataset.dot));
    el.style.width = m.width + "px";
    el.style.height = m.height + "px";
    el.style.webkitMaskImage = el.style.maskImage = "url(" + m.url + ")";
}

// a label: text in dots of dot CSS px (the bar's 2, big buttons' 3 or 4)
function makeLabel(text, dot, className) {
    const el = document.createElement("i");
    el.className = "lbl" + (className ? " " + className : "");
    el.dataset.text = String(text).toUpperCase();
    el.dataset.dot = dot;
    drawLabel(el);
    pad.labels.push(el);
    return el;
}

function setLabel(el, text) {
    if (el.dataset.text !== String(text).toUpperCase()) {
        el.dataset.text = String(text).toUpperCase();
        drawLabel(el);
    }
}

// ---- buttons

// the largest dot a button's words get; narrower buttons get smaller ones (fitLabels)
const LABEL_DOT = { small: 2, medium: 3, big: 4 };

function makeButton(parent, spec, size) {
    const el = document.createElement("div");
    el.className = "key " + (spec.look || "green") + " " + size;
    if (spec.span) {
        el.style.gridColumn = "span " + spec.span;
    }
    const code = padCode(spec);
    const words = String(spec.label).split("\n");
    const box = document.createElement("span");
    box.className = "words";
    for (const w of words) {
        const label = makeLabel(w, LABEL_DOT[size] || 3);
        label.dataset.most = LABEL_DOT[size] || 3;
        box.appendChild(label);
    }
    if (spec.sub) {
        const label = makeLabel(spec.sub, 2, "sub");
        label.dataset.most = 2;
        box.appendChild(label);
    }
    el.appendChild(box);
    const hint = spec.hint !== undefined ? spec.hint : (typeof spec.key === "string" &&
                                                        spec.key.length === 1 ? spec.key : "");
    // no hint that the words already say ("DIE 1" and its 1)
    const last = words[words.length - 1].toUpperCase().split(" ").pop();
    if (hint && hint.toUpperCase() !== last) {
        el.appendChild(makeLabel(hint, 1.34, "hint"));
    }
    el.addEventListener("pointerdown", (e) => {
        e.preventDefault();
        if (el.classList.contains("off")) {
            return; // dim: it would do nothing now
        }
        el.setPointerCapture(e.pointerId);
        el.classList.add("down");
        if (typeof buzz === "function") {
            buzz(); // player.js: off when the bar's BUZZ is off
        } else if (navigator.vibrate) {
            navigator.vibrate(8);
        }
        pad.press(code);
    });
    const up = () => {
        if (el.classList.contains("down")) {
            el.classList.remove("down");
            pad.release(code);
        }
    };
    el.addEventListener("pointerup", up);
    el.addEventListener("pointercancel", up);
    parent.appendChild(el);
    el.padUp = up;
    return el;
}

// the older config: "buttons" in rows of "columns", with "sub" words, "wide" for two
function oldRows(game) {
    const per = game.columns || 4;
    const rows = [];
    let row = null;
    let used = per;
    for (const b of game.buttons || []) {
        const span = b.wide ? 2 : 1;
        if (used + span > per) {
            row = { size: "big", keys: [] };
            rows.push(row);
            used = 0;
        }
        let label = b.label;
        for (const [entity, ch] of Object.entries(PAD_ENTITIES)) {
            label = label.split(entity).join(ch);
        }
        row.keys.push({ label, key: b.key, sub: b.sub || "", span: span > 1 ? span : 0,
                        look: "green" });
        used += span;
    }
    return rows;
}

function buildPad(parent, game) {
    const rows = game.pad || oldRows(game);
    for (const r of rows) {
        const size = r.size || "big";
        const row = document.createElement("div");
        row.className = "row " + size;
        let columns = 0;
        for (const k of r.keys) {
            columns += k.span || 1;
        }
        row.style.gridTemplateColumns = size === "small" ? "" : "repeat(" + columns + ", 1fr)";
        const entry = { el: row, size, keys: [] };
        for (const k of r.keys) {
            const key = { el: makeButton(row, k, size), spec: k, when: null, live: null };
            entry.keys.push(key);
            pad.keys.push(key);
        }
        pad.rows.push(entry);
        parent.appendChild(row);
    }
    return rows.length;
}

function buildKeyboard(parent) {
    const rows = ["1234567890", "QWERTYUIOP", "ASDFGHJKL;", "ZXCVBNM,./"];
    for (const row of rows) {
        for (const ch of row) {
            makeButton(parent, { label: ch, key: ch, look: "green", hint: "" }, "small");
        }
    }
    const extra = [["ESC", "ESC"], ["SPACE", "SPACE", 2], ["←", "LEFT"], ["→", "RIGHT"],
                   ["↑", "UP"], ["↓", "DOWN"], ["DEL", "DELETE"], ["RET", "RETURN", 2]];
    for (const [label, key, span] of extra) {
        makeButton(parent, { label, key, span, look: "green", hint: "" }, "small");
    }
}

// every button's words as big as fit its width, up to their size's dot
function fitLabels() {
    const ratio = window.devicePixelRatio || 1;
    for (const row of document.querySelectorAll("#pad .row.small")) {
        fitSmallRow(row, ratio);
    }
    for (const key of document.querySelectorAll(".key")) {
        if (key.parentElement.classList.contains("small")) {
            continue; // fitSmallRow's
        }
        // the words keep clear of the hint in the corner, on both sides so they stay centred
        let room = key.clientWidth - (key.parentElement.id === "keyboard" ? 6 : 20);
        const hint = key.querySelector(".hint");
        if (hint && !key.classList.contains("small")) {
            room -= 2 * (hint.getBoundingClientRect().width + 6);
        }
        if (room <= 0) {
            continue;
        }
        for (const label of key.querySelectorAll(".words .lbl")) {
            const dots = label.dataset.text.length * 6 - 1;
            let dot = Math.min(Number(label.dataset.most), room / dots);
            dot = Math.max(1, Math.floor(dot * ratio)) / ratio;
            if (Number(label.dataset.dot) !== dot) {
                label.dataset.dot = dot;
                drawLabel(label);
            }
        }
    }
}

// a small row's keys are as wide as their words, so a key squeezed narrow would never grow
// back: every word in the row gets the one dot at which the whole row fits
function fitSmallRow(row, ratio) {
    const keys = Array.from(row.children).filter((key) => !key.hidden);
    if (keys.length === 0 || row.clientWidth === 0) {
        return;
    }
    let fixed = 8 * (keys.length - 1); // the gaps
    let dots = 0;
    let most = 1;
    for (const key of keys) {
        fixed += 24; // the padding
        const hint = key.querySelector(".hint");
        if (hint) {
            fixed += hint.getBoundingClientRect().width + 6;
        }
        let widest = 0;
        for (const label of key.querySelectorAll(".words .lbl")) {
            widest = Math.max(widest, label.dataset.text.length * 6 - 1);
            most = Math.max(most, Number(label.dataset.most));
        }
        dots += widest;
    }
    let dot = Math.min(most, (row.clientWidth - fixed) / Math.max(1, dots));
    dot = Math.max(1, Math.floor(dot * ratio)) / ratio;
    for (const label of row.querySelectorAll(".words .lbl")) {
        if (Number(label.dataset.dot) !== dot) {
            label.dataset.dot = dot;
            drawLabel(label);
        }
    }
}

// ---- layout: which way the screen and the buttons go, and how big the screen is

function layout() {
    const body = document.body;
    const wide = window.innerWidth > window.innerHeight * 1.1 ||
                 (window.matchMedia("(hover: hover) and (pointer: fine)").matches &&
                  window.innerWidth >= 700);
    const taps = Boolean(pad.game && ((pad.game.taps && pad.game.taps.length > 0) || pad.game.mouse));
    const hasPad = Boolean(pad.game) && pad.rows.length > 0;
    body.classList.toggle("wide", wide);
    body.classList.toggle("tall", !wide);
    body.classList.toggle("no-pad", !hasPad || (wide && taps));
    // the room the screen has: across the top when tall, else the stage's box (the main
    // area less the column beside it, when there is one)
    const wrap = document.getElementById("screen-wrap");
    let w = document.getElementById("main").clientWidth;
    let h = w * 384 / 560;
    if (wide) {
        const room = document.getElementById("stage").getBoundingClientRect();
        w = room.width;
        h = room.height;
    }
    const scale = Math.min(w / 560, h / 384);
    wrap.style.width = Math.floor(560 * scale) + "px";
    wrap.style.height = Math.floor(384 * scale) + "px";
    fitLabels();
    fitNote();
}

function fitNote() {
    const note = document.getElementById("note");
    const label = note && note.querySelector(".lbl");
    if (!label || note.clientWidth === 0) {
        return;
    }
    const ratio = window.devicePixelRatio || 1;
    const dots = label.dataset.text.length * 6 - 1;
    let dot = Math.min(Number(label.dataset.most), (note.clientWidth - 24) / dots);
    dot = Math.max(1, Math.floor(dot * ratio)) / ratio;
    if (Number(label.dataset.dot) !== dot) {
        label.dataset.dot = dot;
        drawLabel(label);
    }
}

let padRatio = window.devicePixelRatio;

// the line over the pad's buttons in portrait for a game whose screen takes taps: sideways
// the screen is big enough to tap, and there the pad is gone
function buildNote(game) {
    const note = document.createElement("div");
    note.id = "note";
    document.getElementById("pad").appendChild(note);
    const taps = (game.taps && game.taps.length > 0) || game.mouse;
    const text = game.sideways !== undefined ? game.sideways : "Turn sideways to play by touch";
    if (!taps || !text) {
        return;
    }
    const label = makeLabel(text, 2);
    label.dataset.most = 2;
    note.appendChild(label);
    note.classList.add("on");
}

function padSetup(game, press, release) {
    pad.game = game;
    pad.press = press;
    pad.release = release;
    buildNote(game);
    buildPad(document.getElementById("pad"), game);
    buildKeyboard(document.getElementById("keyboard"));
    pad.follows = compileConditions(game);
    padFollow(null);
    layout();
}

window.addEventListener("resize", () => {
    if (window.devicePixelRatio !== padRatio) {
        padRatio = window.devicePixelRatio;
        for (const el of pad.labels) {
            drawLabel(el);
        }
    }
    layout();
});

// ---- the pad follows the game: which keys show, and which can be pressed, from the game's
// state in the running machine's memory, by the conditions in conditions.js

// every key's when and live; false (and the pad as without them) when the game has no vars
// or a condition does not compile
function compileConditions(game) {
    if (!game.vars) {
        return false;
    }
    const named = conditionNames(game);
    try {
        for (const key of pad.keys) {
            key.when = key.spec.when !== undefined ? compileCondition(key.spec.when, game, named) : null;
            key.live = key.spec.live !== undefined ? compileCondition(key.spec.live, game, named) : null;
        }
    } catch (err) {
        console.warn("pad: " + err.message);
        for (const key of pad.keys) {
            key.when = key.live = null;
        }
        return false;
    }
    return true;
}

// each key: 0 hidden, 1 dim, 2 pressable; read null (the state not known): as without conditions
function keyLooks(read) {
    return pad.keys.map((key) => {
        if (!read || !pad.follows) {
            return key.spec.state_only ? 0 : 2;
        }
        if (key.when && !key.when(read)) {
            return 0;
        }
        return key.live && !key.live(read) ? 1 : 2;
    });
}

// player.js, every frame: read(address, aux) gives a byte of the game's memory, or read is
// null while the game is not running. A change shows once two frames agree, so a state read
// in the middle of the game changing it never flickers the pad
function padFollow(read) {
    let looks;
    try {
        looks = keyLooks(read);
    } catch (err) {
        looks = keyLooks(null);
    }
    const seen = looks.join("");
    if (pad.shown !== null && (seen !== pad.seen || seen === pad.shown)) {
        pad.seen = seen;
        return;
    }
    pad.seen = seen;
    pad.shown = seen;
    pad.keys.forEach((key, i) => {
        const el = key.el;
        if (looks[i] < 2 && el.classList.contains("down")) {
            el.padUp();
        }
        el.hidden = looks[i] === 0;
        el.classList.toggle("off", looks[i] === 1);
    });
    for (const row of pad.rows) {
        let columns = 0;
        for (const key of row.keys) {
            columns += key.el.hidden ? 0 : key.spec.span || 1;
        }
        row.el.hidden = columns === 0;
        if (row.size !== "small" && columns > 0) {
            row.el.style.gridTemplateColumns = "repeat(" + columns + ", 1fr)";
        }
    }
    fitLabels();
}
