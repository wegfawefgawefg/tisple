// The phone player's page: runs machine.wasm (player.cpp) at the Apple's speed, draws its
// screen, plays its sound, turns the game's buttons into key presses, and records every
// press with its machine cycle so a playtest can be replayed exactly on the desktop
// (tisple machine --keys). The game comes from the server (serve.py): games/<name>/game.json
// names its disk, its Mockingboard slot and its buttons.

"use strict";

const CPU_HZ = 14318181.8 * 65 / 912;
const CYCLES_PER_FRAME = 17030;
const AUDIO_RATE = 44100;
const LOOKS = ["color", "ntsc", "mono"];
const KEY_NAMES = { RETURN: 13, ESC: 27, SPACE: 32, LEFT: 8, RIGHT: 21, UP: 11, DOWN: 10,
                    TAB: 9, DELETE: 127 };

const ui = {
    canvas: document.getElementById("screen"),
    start: document.getElementById("start"),
    pad: document.getElementById("pad"),
    keyboard: document.getElementById("keyboard"),
    name: document.getElementById("name"),
    look: document.getElementById("look"),
    kb: document.getElementById("kb"),
    send: document.getElementById("send"),
    rec: document.getElementById("rec"),
    status: document.getElementById("status"),
};
const ctx2d = ui.canvas.getContext("2d");
const image = ctx2d.createImageData(560, 192);

const state = {
    m: null,          // the wasm module
    game: null,       // game.json
    running: false,
    look: 0,
    last: 0,          // performance.now() of the last tick
    audio: null,      // AudioContext
    worklet: null,    // AudioWorkletNode
    log: [],          // [cycle, code] presses, [cycle, -1] releases
    logSent: 0,       // log entries already on the server
    session: Date.now().toString(36),
};

function setStatus(text) {
    ui.status.textContent = text;
}

function keyCode(spec) {
    if (typeof spec === "number") {
        return spec;
    }
    if (KEY_NAMES[spec] !== undefined) {
        return KEY_NAMES[spec];
    }
    return spec.toUpperCase().charCodeAt(0);
}

// input: every press and release goes to the machine and the log

// a browser that kept the sound off at the start tap gets asked again on every press
function wakeAudio() {
    if (state.audio && state.audio.state !== "running") {
        state.audio.resume();
    }
}

function press(code) {
    if (!state.running) {
        return;
    }
    wakeAudio();
    state.m._tp_key_down(code);
    state.log.push([state.m._tp_cycles(), code]);
    ui.rec.textContent = "rec " + state.log.length;
}

function release() {
    if (!state.running) {
        return;
    }
    state.m._tp_key_up();
    state.log.push([state.m._tp_cycles(), -1]);
}

function makeKey(parent, label, code, sub, wide) {
    const el = document.createElement("div");
    el.className = "key" + (wide ? " wide" : "");
    el.innerHTML = label + (sub ? "<small>" + sub + "</small>" : "");
    el.addEventListener("pointerdown", (e) => {
        e.preventDefault();
        el.setPointerCapture(e.pointerId);
        el.classList.add("down");
        if (navigator.vibrate) {
            navigator.vibrate(8);
        }
        press(code);
    });
    const up = () => {
        if (el.classList.contains("down")) {
            el.classList.remove("down");
            release();
        }
    };
    el.addEventListener("pointerup", up);
    el.addEventListener("pointercancel", up);
    parent.appendChild(el);
}

function buildPad(game) {
    ui.pad.style.setProperty("--cols", game.columns || 5);
    for (const b of game.buttons) {
        makeKey(ui.pad, b.label, keyCode(b.key), b.sub, b.wide);
    }
    const rows = ["1234567890", "QWERTYUIOP", "ASDFGHJKL;", "ZXCVBNM,./"];
    for (const row of rows) {
        for (const ch of row) {
            makeKey(ui.keyboard, ch, ch.charCodeAt(0));
        }
    }
    makeKey(ui.keyboard, "esc", 27);
    makeKey(ui.keyboard, "space", 32, "", true);
    makeKey(ui.keyboard, "&larr;", 8);
    makeKey(ui.keyboard, "&rarr;", 21);
    makeKey(ui.keyboard, "&uarr;", 11);
    makeKey(ui.keyboard, "&darr;", 10);
    makeKey(ui.keyboard, "del", 127);
    makeKey(ui.keyboard, "ret", 13, "", true);
}

// taps on the screen itself: the game's taps (game.json "taps") are boxes in hi-res pixels,
// 280 x 192, each with the key it presses
let held_tap = null;

function tapAt(e) {
    const r = ui.canvas.getBoundingClientRect();
    const x = (e.clientX - r.left) / r.width * 280;
    const y = (e.clientY - r.top) / r.height * 192;
    for (const t of state.game.taps || []) {
        if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h) {
            return t;
        }
    }
    return null;
}

function flashTap(t) {
    const r = ui.canvas.getBoundingClientRect();
    const box = document.createElement("div");
    box.className = "flash";
    box.style.left = (t.x / 280 * r.width) + "px";
    box.style.top = (t.y / 192 * r.height) + "px";
    box.style.width = (t.w / 280 * r.width) + "px";
    box.style.height = (t.h / 192 * r.height) + "px";
    ui.canvas.parentElement.appendChild(box);
    setTimeout(() => box.remove(), 150);
}

ui.canvas.addEventListener("pointerdown", (e) => {
    e.preventDefault();
    const t = tapAt(e);
    if (!t) {
        return;
    }
    ui.canvas.setPointerCapture(e.pointerId);
    held_tap = t;
    flashTap(t);
    if (navigator.vibrate) {
        navigator.vibrate(8);
    }
    press(keyCode(t.key));
});
const tapUp = () => {
    if (held_tap) {
        held_tap = null;
        release();
    }
};
ui.canvas.addEventListener("pointerup", tapUp);
ui.canvas.addEventListener("pointercancel", tapUp);

// a desktop keyboard works too, for trying the page out
window.addEventListener("keydown", (e) => {
    if (e.repeat || e.ctrlKey || e.metaKey) {
        return;
    }
    const named = { Enter: 13, Escape: 27, ArrowLeft: 8, ArrowRight: 21, ArrowUp: 11,
                    ArrowDown: 10, Backspace: 127, Tab: 9 };
    let code = named[e.key];
    if (code === undefined && e.key.length === 1) {
        code = e.key.toUpperCase().charCodeAt(0);
    }
    if (code !== undefined) {
        e.preventDefault();
        press(code);
    }
});
window.addEventListener("keyup", () => release());

// sound: an AudioWorklet plays a queue of samples the machine made

const WORKLET = `
class Feed extends AudioWorkletProcessor {
    constructor() {
        super();
        this.queue = [];
        this.offset = 0;
        this.port.onmessage = (e) => {
            this.queue.push(e.data);
            let total = -this.offset;
            for (const q of this.queue) total += q.length;
            // more than 0.25 s behind: drop the oldest so sound stays with the picture
            while (total > 11025 && this.queue.length > 1) {
                total -= this.queue[0].length - this.offset;
                this.queue.shift();
                this.offset = 0;
            }
        };
    }
    process(inputs, outputs) {
        const out = outputs[0][0];
        for (let i = 0; i < out.length; i++) {
            if (this.queue.length === 0) {
                out[i] = 0;
                continue;
            }
            const q = this.queue[0];
            out[i] = q[this.offset++];
            if (this.offset >= q.length) {
                this.queue.shift();
                this.offset = 0;
            }
        }
        return true;
    }
}
registerProcessor("feed", Feed);
`;

async function startAudio() {
    state.audio = new AudioContext({ sampleRate: AUDIO_RATE, latencyHint: "interactive" });
    // resume inside the tap itself: after an await, some browsers (Brave) no longer count the
    // gesture and keep the context suspended
    state.audio.resume();
    if (state.audio.audioWorklet) {
        const url = URL.createObjectURL(new Blob([WORKLET], { type: "text/javascript" }));
        await state.audio.audioWorklet.addModule(url);
        state.worklet = new AudioWorkletNode(state.audio, "feed");
        state.worklet.connect(state.audio.destination);
        return;
    }
    // plain http (no secure context, no AudioWorklet): the older ScriptProcessor, same queue
    state.fallback = { queue: [], offset: 0 };
    const node = state.audio.createScriptProcessor(1024, 0, 1);
    node.onaudioprocess = (e) => {
        const out = e.outputBuffer.getChannelData(0);
        const f = state.fallback;
        for (let i = 0; i < out.length; i++) {
            if (f.queue.length === 0) {
                out[i] = 0;
                continue;
            }
            out[i] = f.queue[0][f.offset++];
            if (f.offset >= f.queue[0].length) {
                f.queue.shift();
                f.offset = 0;
            }
        }
    };
    node.connect(state.audio.destination);
    state.worklet = { port: { postMessage: (pcm) => {
        const f = state.fallback;
        f.queue.push(pcm);
        let total = -f.offset;
        for (const q of f.queue) total += q.length;
        while (total > 11025 && f.queue.length > 1) {
            total -= f.queue[0].length - f.offset;
            f.queue.shift();
            f.offset = 0;
        }
    } } };
}

async function startAudioOrNot() {
    try {
        await startAudio();
    } catch (err) {
        state.audio = null;
        state.worklet = null;
        setStatus("no sound: " + err.message);
    }
}

function sendSamples(count) {
    if (!state.worklet || count === 0) {
        return;
    }
    const ptr = state.m._tp_samples() >> 1;
    const pcm = new Float32Array(count);
    for (let i = 0; i < count; i++) {
        pcm[i] = state.m.HEAP16[ptr + i] / 32768;
    }
    state.worklet.port.postMessage(pcm, [pcm.buffer]);
}

// the loop: run the machine for the time that passed, then draw

function draw() {
    const ptr = state.m._tp_screen(state.look);
    image.data.set(state.m.HEAPU8.subarray(ptr, ptr + 560 * 192 * 4));
    ctx2d.putImageData(image, 0, 0);
}

function tick(now) {
    requestAnimationFrame(tick);
    if (!state.running) {
        return;
    }
    if (state.m._tp_disk_busy()) {
        // loading: flat out for most of a frame, silent
        const until = performance.now() + 12;
        while (state.m._tp_disk_busy() && performance.now() < until) {
            state.m._tp_run(CYCLES_PER_FRAME * 4, 0);
        }
        state.last = now;
        draw();
        setStatus("loading...");
        return;
    }
    let seconds = (now - state.last) / 1000;
    state.last = now;
    if (seconds > 0.1) {
        seconds = 0.1; // the tab was asleep: don't try to catch up
    }
    const cycles = Math.round(seconds * CPU_HZ);
    if (cycles > 0) {
        const count = state.m._tp_run(cycles, state.audio ? 1 : 0);
        sendSamples(count);
    }
    draw();
    if (state.audio && state.audio.state !== "running") {
        setStatus("sound is " + state.audio.state + ": tap a button");
    } else if (state.m._tp_stopped()) {
        setStatus("the program stopped");
    } else if (ui.status.textContent === "loading...") {
        setStatus("");
    }
}

// playtest logs go to the server, which keeps them for replay

async function sendLog(final) {
    if (state.log.length === state.logSent) {
        return;
    }
    const body = JSON.stringify({
        game: state.game.name, version: state.game.version || "", session: state.session,
        mockingboard: state.game.mockingboard || 0, events: state.log,
    });
    try {
        if (final && navigator.sendBeacon) {
            navigator.sendBeacon("log", body);
        } else {
            await fetch("log", { method: "POST", body, headers: { "Content-Type": "application/json" } });
        }
        state.logSent = state.log.length;
        setStatus("log sent (" + state.log.length + " events)");
    } catch (err) {
        setStatus("log not sent: " + err.message);
    }
}

document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "hidden") {
        sendLog(true);
    }
});
setInterval(() => sendLog(false), 30000);

// start: the game's config and disk, then wait for a tap (browsers start sound only on one)

async function boot() {
    const params = new URLSearchParams(location.search);
    const index = await (await fetch("games/index.json", { cache: "no-store" })).json();
    const name = params.get("game") || index.default;
    state.game = await (await fetch("games/" + name + "/game.json", { cache: "no-store" })).json();
    state.game.name = name;
    ui.name.textContent = state.game.title || name;
    document.title = state.game.title || name;
    buildPad(state.game);
    const disk = new Uint8Array(await (await fetch("games/" + name + "/" + state.game.disk,
                                                   { cache: "no-store" })).arrayBuffer());
    state.m = await TispleMachine();
    const ptr = state.m._malloc(disk.length);
    state.m.HEAPU8.set(disk, ptr);
    const ok = state.m._tp_boot(ptr, disk.length, state.game.mockingboard || 0);
    state.m._free(ptr);
    if (!ok) {
        setStatus("the disk did not load");
        return;
    }
    setStatus(state.game.version ? "build " + state.game.version : "");
    requestAnimationFrame(tick);
    // ?autostart: run without the tap and without sound (headless checks)
    if (params.has("autostart")) {
        ui.start.style.display = "none";
        state.last = performance.now();
        state.running = true;
    }
}

// keeps the screen on while the page is in front (https only; dropped when hidden)
async function keepAwake() {
    try {
        if (navigator.wakeLock && document.visibilityState === "visible") {
            await navigator.wakeLock.request("screen");
        }
    } catch (err) {
        // no wake lock over plain http: the screen may dim
    }
}
document.addEventListener("visibilitychange", () => {
    if (state.running) {
        keepAwake();
    }
});

ui.start.addEventListener("pointerdown", async () => {
    if (!state.m) {
        return;
    }
    ui.start.style.display = "none";
    keepAwake();
    await startAudioOrNot();
    state.last = performance.now();
    state.running = true;
});
ui.look.addEventListener("click", () => {
    state.look = (state.look + 1) % LOOKS.length;
    ui.look.textContent = LOOKS[state.look];
});
ui.kb.addEventListener("click", () => ui.keyboard.classList.toggle("open"));
ui.send.addEventListener("click", () => sendLog(false));

boot().catch((err) => setStatus("could not start: " + err.message));
