// The phone player's page: runs machine.wasm (player.cpp) at the Apple's speed, draws its
// screen, plays its sound, turns the game's buttons into key presses, and records every
// press with its machine cycle so a playtest can be replayed exactly on the desktop
// (tisple machine --keys). The game comes from the server (serve.py): games/<name>/game.json
// names its disk, its Mockingboard slot and its buttons. On a static site (web/pages.py,
// games/index.json has "static": true) there is no server to take logs: no log, and a
// games button goes back to the picker. The buttons, the bar's letters and the layout are
// pad.js's. The disk button turns the drives' sound on and off (remembered in this browser),
// off until it is pressed (the owner, 2026-10-31: opt in): on, a disk loads at the Apple's
// speed and sounds like a Disk II; off, it loads flat out and silent. A game whose game.json
// has "telemetry" sends anonymous play records (telemetry.js), the bar's STATS button turns
// them off and on.

"use strict";

const CPU_HZ = 14318181.8 * 65 / 912;
const CYCLES_PER_FRAME = 17030;
const AUDIO_RATE = 44100;
const LOOKS = ["color", "ntsc", "mono"];

const ui = {
    canvas: document.getElementById("screen"),
    start: document.getElementById("start"),
    keyboard: document.getElementById("keyboard"),
    side: document.getElementById("side"),
    name: document.getElementById("name"),
    look: document.getElementById("look"),
    disk: document.getElementById("disk"),
    buzz: document.getElementById("buzz"),
    stats: document.getElementById("stats"),
    kb: document.getElementById("kb"),
    send: document.getElementById("send"),
    games: document.getElementById("games"),
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
    log: [],          // [cycle, code] presses, [cycle, -1] releases, [cycle, -2, x, y, down] taps
    logSent: 0,       // log entries already on the server
    session: Date.now().toString(36),
    logs: true,       // false on a static site: nowhere to post them
    buzz: true,       // a short buzz on each tap (the bar's BUZZ button turns it off)
    diskSound: true, // the drives' sound, and loading at the Apple's speed to hear it (on by default: the owner, 2026-11-01)
};

// the disk button's setting, kept in this browser when it can be
const DISK_SOUND_KEY = "tisple.diskSound";
try {
    state.diskSound = localStorage.getItem(DISK_SOUND_KEY) !== "off";
} catch (err) {
    // no storage (private window, blocked): on, as by default
}

// the buzz button's setting, kept in this browser when it can be
const BUZZ_KEY = "tisple.buzz";
try {
    state.buzz = localStorage.getItem(BUZZ_KEY) !== "off";
} catch (err) {
    // no storage: on, as by default
}
function buzz() {
    if (state.buzz && navigator.vibrate) {
        navigator.vibrate(8);
    }
}

// the bar's and the start's words, in the machine's letters (pad.js)
const words = {
    name: makeLabel("Tisple ][", 2),
    rec: makeLabel("", 2),
    look: makeLabel(LOOKS[0], 2),
    disk: makeLabel("disk", 2),
    buzz: makeLabel("buzz", 2),
    stats: makeLabel("stats", 2),
    kb: makeLabel("abc", 2),
    send: makeLabel("send log", 2),
    games: makeLabel("games", 2),
    start: makeLabel("tap to start", 3),
    status: makeLabel("", 1),
};
for (const key of Object.keys(words)) {
    ui[key].appendChild(words[key]);
}

function setStatus(text) {
    setLabel(words.status, text);
}

// input: every press and release goes to the machine and the log

// a browser that kept the sound off at the start tap gets asked again on every press
function wakeAudio() {
    if (state.audio && state.audio.state !== "running") {
        state.audio.resume();
    }
}

// the game port from the pad (pad.js padCode): a stick way to its paddle's end, a button
function gamePort(code, down) {
    if (code >= 0x200) {
        state.m._tp_button(code - 0x200, down ? 1 : 0);
        return;
    }
    const way = code - 0x100;
    state.m._tp_paddle(way < 2 ? 0 : 1, down ? (way % 2 === 1 ? 255 : 0) : 127);
}

function press(code) {
    if (!state.running) {
        return;
    }
    wakeAudio();
    if (code >= 0x100) {
        gamePort(code, true); // not in the log, which replays keys
        return;
    }
    state.m._tp_key_down(code);
    state.log.push([state.m._tp_cycles(), code]);
    if (state.logs) {
        setLabel(words.rec, "rec " + state.log.length);
    }
}

function release(code) {
    if (!state.running) {
        return;
    }
    if (code >= 0x100) {
        gamePort(code, false);
        return;
    }
    state.m._tp_key_up();
    state.log.push([state.m._tp_cycles(), -1]);
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

// a game whose config says "mouse": true has a mouse card instead of taps: a tap puts the
// mouse where it is with its button down until the finger lifts (player.cpp tp_point), and the
// game knows what it drew there. In the log as [cycle, -2, x, y, down]
let pointed = null;

function pointAt(e) {
    const r = ui.canvas.getBoundingClientRect();
    return {
        x: Math.max(0, Math.min(279, Math.floor((e.clientX - r.left) / r.width * 280))),
        y: Math.max(0, Math.min(191, Math.floor((e.clientY - r.top) / r.height * 192))),
    };
}

function point(p, down) {
    if (!state.running) {
        return;
    }
    wakeAudio();
    state.m._tp_point(p.x, p.y, down ? 1 : 0);
    state.log.push([state.m._tp_cycles(), -2, p.x, p.y, down ? 1 : 0]);
}

ui.canvas.addEventListener("pointerdown", (e) => {
    e.preventDefault();
    if (state.game && state.game.mouse) {
        ui.canvas.setPointerCapture(e.pointerId);
        pointed = pointAt(e);
        buzz();
        point(pointed, true);
        return;
    }
    const t = tapAt(e);
    if (!t) {
        return;
    }
    ui.canvas.setPointerCapture(e.pointerId);
    held_tap = t;
    buzz();
    press(padKeyCode(t.key));
});
const tapUp = () => {
    if (pointed) {
        point(pointed, false);
        pointed = null;
    }
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

// saves: what a program writes to its disk (lib/save) is kept in this browser, as the 256-byte
// blocks of the .dsk that differ from the published disk, under the game's name. At boot each
// kept block goes back onto the disk where the published one is all zeros there (an empty
// save area): so saves outlive a new build of the game while its save areas stay where they
// were, and never overwrite its program or its other tables.

const SAVES_KEY = "tisple.saves.";

function toBase64(bytes) {
    let text = "";
    for (let i = 0; i < bytes.length; i++) {
        text += String.fromCharCode(bytes[i]);
    }
    return btoa(text);
}

function fromBase64(text) {
    const raw = atob(text);
    const bytes = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i++) {
        bytes[i] = raw.charCodeAt(i);
    }
    return bytes;
}

function allZero(bytes, at) {
    for (let i = 0; i < 256; i++) {
        if (bytes[at + i] !== 0) {
            return false;
        }
    }
    return true;
}

// the kept blocks onto the published disk (a copy)
function withSaves(disk) {
    let kept = null;
    try {
        kept = JSON.parse(localStorage.getItem(SAVES_KEY + state.game.name) || "null");
    } catch (err) {
        kept = null;
    }
    if (!kept) {
        return disk;
    }
    const out = new Uint8Array(disk);
    for (const at of Object.keys(kept)) {
        const offset = Number(at);
        const block = fromBase64(kept[at]);
        if (block.length === 256 && offset + 256 <= out.length && allZero(disk, offset)) {
            out.set(block, offset);
        }
    }
    return out;
}

// the disk's blocks that differ from the published one, kept
function keepSaves() {
    if (!state.m._tp_disk_written()) {
        return;
    }
    const ptr = state.m._tp_disk_image();
    if (!ptr) {
        return;
    }
    const image = state.m.HEAPU8.subarray(ptr, ptr + state.m._tp_disk_image_size());
    const kept = {};
    for (let at = 0; at + 256 <= image.length && at + 256 <= state.disk.length; at += 256) {
        let same = true;
        for (let i = 0; i < 256 && same; i++) {
            same = image[at + i] === state.disk[at + i];
        }
        if (!same) {
            kept[at] = toBase64(image.subarray(at, at + 256));
        }
    }
    try {
        localStorage.setItem(SAVES_KEY + state.game.name, JSON.stringify(kept));
    } catch (err) {
        setStatus("save not kept: " + err.message);
    }
}

setInterval(() => {
    if (state.m && state.running) {
        keepSaves();
    }
}, 1000);

// the loop: run the machine for the time that passed, then draw

function draw() {
    const ptr = state.m._tp_screen(state.look);
    image.data.set(state.m.HEAPU8.subarray(ptr, ptr + 560 * 192 * 4));
    ctx2d.putImageData(image, 0, 0);
}

// the game's state for the pad (pad.js padFollow) and the play records (telemetry.js): its
// memory once it is running, which is once its first bytes are in memory and the disk has
// stopped after the boot; until the bytes change again (a reboot)
let gameRunning = false;

function readRam(address, aux) {
    return state.m._tp_peek_ram(address, aux);
}

function followGame() {
    const check = state.game.check;
    if (!check || !(pad.follows || tel.events.length)) {
        return;
    }
    let loaded = true;
    for (let i = 0; i < check.bytes.length && loaded; i++) {
        loaded = readRam(check.at + i, 0) === check.bytes[i];
    }
    if (!loaded) {
        gameRunning = false;
    } else if (!state.m._tp_disk_busy()) {
        gameRunning = true;
    }
    if (pad.follows) {
        padFollow(gameRunning ? readRam : null);
    }
    telemetryFrame(gameRunning ? readRam : null);
}

function tick(now) {
    requestAnimationFrame(tick);
    if (!state.running) {
        return;
    }
    followGame();
    const busy = state.m._tp_disk_busy();
    const hearing = state.diskSound && state.audio && state.audio.state === "running";
    if (busy && !hearing) {
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
    } else if (busy) {
        setStatus("loading...");
    } else if (words.status.dataset.text === "LOADING...") {
        setStatus("");
    }
}

// playtest logs go to the server, which keeps them for replay

async function sendLog(final) {
    if (!state.logs || state.log.length === state.logSent) {
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
    if (index.static) {
        state.logs = false;
        ui.send.hidden = true;
        ui.games.hidden = false;
    }
    state.game = await (await fetch("games/" + name + "/game.json", { cache: "no-store" })).json();
    state.game.name = name;
    setLabel(words.name, state.game.title || name);
    // "look": the colour look the game was drawn for, until the viewer changes it
    if (LOOKS.indexOf(state.game.look) >= 0) {
        state.look = LOOKS.indexOf(state.game.look);
        setLabel(words.look, LOOKS[state.look]);
    }
    document.title = state.game.title || name;
    padSetup(state.game, press, release);
    const telemetry = telemetrySetup(state.game, {
        game: name, build: state.game.version, where: index.static ? "pages" : "phone",
        cycles: () => (state.m ? state.m._tp_cycles() : 0), hz: CPU_HZ });
    ui.stats.hidden = !telemetry;
    showStats();
    const fetchDisk = async (file) =>
        new Uint8Array(await (await fetch("games/" + name + "/" + file,
                                          { cache: "no-store" })).arrayBuffer());
    const disk = await fetchDisk(state.game.disk);
    // "disk2": a program with tables on a second disk (docs/disks.md) has it in drive 2
    const disk2 = state.game.disk2 ? await fetchDisk(state.game.disk2) : new Uint8Array(0);
    state.m = await TispleMachine();
    state.disk = disk;
    const booted = withSaves(disk);
    const ptr = state.m._malloc(booted.length);
    state.m.HEAPU8.set(booted, ptr);
    const ptr2 = state.m._malloc(Math.max(disk2.length, 1));
    state.m.HEAPU8.set(disk2, ptr2);
    const ok = state.m._tp_boot(ptr, booted.length, ptr2, disk2.length,
                                state.game.mockingboard || 0);
    state.m._free(ptr);
    state.m._free(ptr2);
    if (!ok) {
        setStatus("the disk did not load");
        return;
    }
    if (state.game.mouse && !state.m._tp_mouse_card()) {
        setStatus("no mouse: the mockingboard is in its slot 4");
    }
    showDiskSound();
    setStatus(state.game.version ? "build " + state.game.version : "");
    requestAnimationFrame(tick);
    // ?autostart: run without the tap and without sound (headless checks)
    if (params.has("autostart")) {
        ui.start.style.display = "none";
        state.last = performance.now();
        state.running = true;
        telemetryOpen();
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
    telemetryOpen();
});
ui.look.addEventListener("click", () => {
    state.look = (state.look + 1) % LOOKS.length;
    setLabel(words.look, LOOKS[state.look]);
});
function showDiskSound() {
    ui.disk.classList.toggle("off", !state.diskSound);
    if (state.m) {
        state.m._tp_drive_sound(state.diskSound ? 1 : 0);
    }
}
ui.disk.addEventListener("click", () => {
    state.diskSound = !state.diskSound;
    try {
        localStorage.setItem(DISK_SOUND_KEY, state.diskSound ? "on" : "off");
    } catch (err) {
        // not kept: this page only
    }
    showDiskSound();
    setStatus(state.diskSound ? "disk sound on: loads at the apple's speed"
                              : "disk sound off: loads flat out");
});
function showBuzz() {
    ui.buzz.classList.toggle("off", !state.buzz);
}
showBuzz();
ui.buzz.addEventListener("click", () => {
    state.buzz = !state.buzz;
    try {
        localStorage.setItem(BUZZ_KEY, state.buzz ? "on" : "off");
    } catch (err) {
        // not kept: this page only
    }
    showBuzz();
    setStatus(state.buzz ? "buzz on: taps buzz" : "buzz off: taps are silent");
});
// the STATS button: anonymous play records on or off (telemetry.js). The bar has no room for it
// on a phone held upright (index.html shows it from 480 px across), so the start's tap has it
// too, under "tap to start", saying what is sent: a tap there turns them off (or on) and does
// not start
const startStats = document.createElement("span");
startStats.className = "stats";
const startNote = makeLabel("", 1);
startStats.appendChild(startNote);
ui.start.appendChild(startStats);
function showStats() {
    const t = telemetryShown();
    ui.stats.classList.toggle("off", !t.on);
    startStats.hidden = !t.has;
    startStats.classList.toggle("off", !t.on);
    setLabel(startNote, t.on ? "sends anonymous play stats: tap here to stop"
                              : t.blocked ? "play stats off (" + t.blocked + ")"
                                          : "play stats off: tap here to send them");
}
function toggleStats() {
    const t = telemetryShown();
    if (t.blocked) {
        setStatus("play stats off: the browser asks for " + t.blocked);
        return;
    }
    const on = telemetrySet(!t.on);
    showStats();
    setStatus(on ? "play stats on: anonymous, how far runs get" : "play stats off: nothing is sent");
}
ui.stats.addEventListener("click", toggleStats);
startStats.addEventListener("pointerdown", (e) => {
    e.stopPropagation(); // not the start
    toggleStats();
});
ui.kb.addEventListener("click", () => {
    ui.keyboard.classList.toggle("open");
    ui.side.classList.toggle("kb", ui.keyboard.classList.contains("open"));
    layout();
});
ui.send.addEventListener("click", () => sendLog(false));

boot().catch((err) => setStatus("could not start: " + err.message));
