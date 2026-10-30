// The player's controls and layout, drawn in the Apple II's own letters (font.js, from the
// machine's font): the bar, the game's buttons (game.json "pad", or the older "buttons"),
// the abc keyboard, and where the screen goes. Portrait: the screen on top, the buttons under
// it, the most used ones biggest and lowest, in thumb reach. Landscape and desktop: the screen
// as large as fits; a game whose screen takes taps (game.json "taps") gets no buttons there,
// any other has them in a column beside it. player.js owns the machine and calls press and
// release, which it gives to padSetup.
//
// "pad" is a list of rows, top to bottom:
//   {"size": "small" | "medium" | "big", "keys": [{"label": "END TURN", "key": "E",
//    "look": "card" | "white" | "violet" | "orange" | "green", "span": 2, "hint": "E",
//    "sub": "smaller words under the label"}]}
// A key is a character, a number, or RETURN ESC SPACE LEFT RIGHT UP DOWN TAB DELETE; the hint
// (the key letter, by default) shows small in a corner. Small rows sit at the right, out of
// the way.

"use strict";

const PAD_KEY_NAMES = { RETURN: 13, ESC: 27, SPACE: 32, LEFT: 8, RIGHT: 21, UP: 11, DOWN: 10,
                        TAB: 9, DELETE: 127 };
const PAD_ENTITIES = { "&larr;": "←", "&rarr;": "→", "&uarr;": "↑",
                       "&darr;": "↓" };

const pad = {
    press: null,   // player.js: press(code), release()
    release: null,
    game: null,
    labels: [],    // every label element, redrawn when the device pixel ratio changes
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
    const code = padKeyCode(spec.key);
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
        el.setPointerCapture(e.pointerId);
        el.classList.add("down");
        if (navigator.vibrate) {
            navigator.vibrate(8);
        }
        pad.press(code);
    });
    const up = () => {
        if (el.classList.contains("down")) {
            el.classList.remove("down");
            pad.release();
        }
    };
    el.addEventListener("pointerup", up);
    el.addEventListener("pointercancel", up);
    parent.appendChild(el);
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
        for (const k of r.keys) {
            makeButton(row, k, size);
        }
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
    for (const key of document.querySelectorAll(".key")) {
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

// ---- layout: which way the screen and the buttons go, and how big the screen is

function layout() {
    const body = document.body;
    const wide = window.innerWidth > window.innerHeight * 1.1 ||
                 (window.matchMedia("(hover: hover) and (pointer: fine)").matches &&
                  window.innerWidth >= 700);
    const taps = Boolean(pad.game && pad.game.taps && pad.game.taps.length > 0);
    const hasPad = Boolean(pad.game) && document.getElementById("pad").childElementCount > 0;
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
}

let padRatio = window.devicePixelRatio;

function padSetup(game, press, release) {
    pad.game = game;
    pad.press = press;
    pad.release = release;
    buildPad(document.getElementById("pad"), game);
    buildKeyboard(document.getElementById("keyboard"));
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
