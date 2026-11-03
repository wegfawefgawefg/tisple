// Play telemetry: small anonymous records of how a game is played, posted to the endpoint its
// game.json names, made from conditions over the running game's memory as the pad's are
// (conditions.js): a game opts in with config alone, no code and no memory of its own.
//
//   "telemetry": {
//     "endpoint": "https://45.77.123.14/catwiz-telemetry",
//     "fields": {"stage": "state.stage", "kit": "state.items", "won": "state.phase == state.Phase.won"},
//     "events": [
//       {"name": "fight_start", "when": "in_fight", "fields": ["stage", "kit"]},
//       {"name": "pick", "change": ["reward", "state.pick"], "was": "reward",
//        "entry": ["kit"], "before": ["offered"], "fields": ["kit"]}
//     ],
//     "quit": ["stage"]
//   }
//
// A field is a condition's number, or a var named alone with more than one element: the list of
// them (compileField). An event with "when" fires as that starts to hold (also when it holds at
// the first look); one with "change" (a condition or a list) whenever its values change, but not
// at the first look; "was", a condition that held just before (with "change": what the state was
// leaving). Its "fields" are read as it fires, its "before" as they were just before, its
// "entry" as they were when the state it leaves began (a kit before a pick changed it). A state
// counts once two frames agree, as the pad's, so a read in the middle of the game changing it
// counts for nothing. Built in: "open" (the start tap), "quit" (the page hidden or closed, with
// the "quit" fields: sent at once with sendBeacon; the last one a session has is where it quit),
// "back" (shown again after a quit).
//
// Each record: e the event, t seconds since the page loaded, dt since the record before, gt the
// machine's seconds (it stops while the page is hidden), v the fields, pre the before ones,
// entry the entry ones.
// A post: game, build (game.json version), where ("pages" for the static site, else "phone"),
// sid (a random id made once a tab session, kept in sessionStorage), load (the page loads in
// that session), seq (the posts this load), recs. Sent as text/plain, a simple request (no CORS
// preflight), every FLUSH_MS and as the page hides. Nothing else: no address, no browser, no
// screen. Off with the bar's STATS button or ?telemetry=off (kept in localStorage; on again with
// ?telemetry=on), and always off with Do Not Track or Global Privacy Control. Every failure is
// silent and loses at most the records: the game never waits on it.

"use strict";

const TELEMETRY_KEY = "tisple.telemetry";
const TELEMETRY_SID_KEY = "tisple.telemetry.sid";
const TELEMETRY_LOAD_KEY = "tisple.telemetry.load";
const TELEMETRY_FLUSH_MS = 20000;
const TELEMETRY_KEEP = 200; // records waiting at most (the oldest go)
const TELEMETRY_POST = 60;  // records a post at most

const tel = {
    config: null,   // game.json "telemetry", when it has one with an endpoint
    on: false,      // sending (configured, allowed, not turned off)
    blocked: "",    // why it can't be turned on: Do Not Track or Global Privacy Control
    events: [],     // compiled: {name, trig, change, was, fields, before, stable, seen, pre, ...}
    quit: [],       // the quit record's fields, compiled
    fields: {},     // every field compiled, by name
    pending: [],    // records not sent yet
    sid: "",
    load: 0,
    seq: 0,
    last: 0,        // t of the record before
    head: null,     // game, build, where
    cycles: null,   // the machine's cycle count now (player.js)
    hz: 1,
    read: null,     // the last frame's read, for a quit's fields
    quitted: false, // a quit sent and the page not shown again yet
    opened: false,
};

function telStore(store, key, value) {
    try {
        if (value === undefined) {
            return store.getItem(key);
        }
        store.setItem(key, value);
    } catch (err) {
        // no storage (private window, blocked): this page only
    }
    return null;
}

function telRandomId() {
    const bytes = new Uint8Array(8);
    crypto.getRandomValues(bytes); // also over plain http, unlike randomUUID
    return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

// the browser asks not to be tracked: off, whatever was chosen
function telBlocked() {
    if (navigator.globalPrivacyControl === true) {
        return "global privacy control";
    }
    const dnt = navigator.doNotTrack || window.doNotTrack || navigator.msDoNotTrack;
    if (dnt === "1" || dnt === "yes") {
        return "do not track";
    }
    return "";
}

// ?telemetry=off / on, kept in this browser; then the kept choice; on by default
function telChosen() {
    const asked = new URLSearchParams(location.search).get("telemetry");
    if (asked === "off" || asked === "on") {
        telStore(localStorage, TELEMETRY_KEY, asked);
    }
    return telStore(localStorage, TELEMETRY_KEY) !== "off";
}

// game.json read: compile its telemetry. info: {game, build, where, cycles(), hz}. Gives
// whether the game has telemetry (the bar shows the STATS button then)
function telemetrySetup(game, info) {
    const config = game.telemetry;
    // https, or this machine (a test receiver)
    if (!config || typeof config.endpoint !== "string" ||
        !/^(https:\/\/|http:\/\/(127\.0\.0\.1|localhost)[:/])/.test(config.endpoint)) {
        return false;
    }
    tel.config = config;
    tel.head = { game: info.game, build: String(info.build || "").slice(0, 40), where: info.where };
    tel.cycles = info.cycles;
    tel.hz = info.hz;
    tel.blocked = telBlocked();
    tel.on = !tel.blocked && telChosen();
    if (game.vars) {
        try {
            telCompile(game, config);
        } catch (err) {
            console.warn("telemetry: " + err.message); // only the built-in records then
            tel.events = [];
            tel.quit = [];
        }
    }
    tel.sid = telStore(sessionStorage, TELEMETRY_SID_KEY);
    if (!tel.sid || !/^[0-9a-f]{16}$/.test(tel.sid)) {
        tel.sid = telRandomId();
        telStore(sessionStorage, TELEMETRY_SID_KEY, tel.sid);
    }
    tel.load = (Number(telStore(sessionStorage, TELEMETRY_LOAD_KEY)) || 0) + 1;
    telStore(sessionStorage, TELEMETRY_LOAD_KEY, String(tel.load));
    return true;
}

function telCompile(game, config) {
    const named = conditionNames(game);
    const field = (name) => {
        if (!tel.fields[name]) {
            const text = config.fields && config.fields[name] !== undefined ? config.fields[name] : name;
            tel.fields[name] = compileField(text, game, named);
        }
        return { name, read: tel.fields[name] };
    };
    const list = (names) => (names || []).map(field);
    for (const spec of config.events || []) {
        const ev = { name: String(spec.name), fields: list(spec.fields), before: list(spec.before),
                     entry: list(spec.entry), atEntry: null,
                     was: spec.was !== undefined ? compileCondition(spec.was, game, named) : null,
                     stable: null, seen: null, pre: null, preSeen: "", wasHeld: true };
        if (spec.change !== undefined) {
            const parts = (Array.isArray(spec.change) ? spec.change : [spec.change])
                .map((c) => compileCondition(c, game, named));
            ev.change = true;
            ev.trig = (read) => parts.map((p) => p(read)).join(",");
        } else {
            const when = compileCondition(spec.when, game, named);
            ev.change = false;
            ev.stable = "0"; // a when that holds at the first look fires
            ev.trig = (read) => (when(read) ? "1" : "0");
        }
        tel.events.push(ev);
    }
    tel.quit = list(config.quit);
}

function telValues(fields, read) {
    if (!fields.length || !read) {
        return null;
    }
    const out = {};
    for (const f of fields) {
        out[f.name] = f.read(read);
    }
    return out;
}

function telRecord(name, values, before, entry) {
    if (!tel.on) {
        return;
    }
    const t = Math.round(performance.now() / 100) / 10;
    const rec = { e: name, t, dt: Math.round((t - tel.last) * 10) / 10,
                  gt: Math.round(tel.cycles() / tel.hz * 10) / 10 };
    if (values) {
        rec.v = values;
    }
    if (before) {
        rec.pre = before;
    }
    if (entry) {
        rec.entry = entry;
    }
    tel.last = t;
    tel.pending.push(rec);
    if (tel.pending.length > TELEMETRY_KEEP) {
        tel.pending.splice(0, tel.pending.length - TELEMETRY_KEEP);
    }
}

// player.js, every frame: read(address, aux) gives a byte of the game's memory, or read is null
// while the game is not running
function telemetryFrame(read) {
    if (!tel.on || !read) {
        return;
    }
    tel.read = read;
    try {
        for (const ev of tel.events) {
            telEvent(ev, read);
        }
    } catch (err) {
        // a read gone wrong: nothing recorded this frame
    }
}

function telEvent(ev, read) {
    const k = ev.trig(read);
    if (k !== ev.seen) {
        ev.seen = k; // a change counts once the next frame agrees
        return;
    }
    if (k === ev.stable) {
        // the state holding: what it was (before, was) for when it changes
        if (ev.before.length) {
            const b = telValues(ev.before, read);
            const text = JSON.stringify(b);
            if (text === ev.preSeen) {
                ev.pre = b;
            }
            ev.preSeen = text;
        }
        if (ev.was) {
            ev.wasHeld = ev.was(read) !== 0;
        }
        return;
    }
    // a new state: an event unless it is the first look (null: a change event's, or any after
    // being turned on again), or a when that stopped holding, or not what "was" asks for
    const first = ev.stable === null;
    const pre = ev.pre;
    const entry = ev.atEntry;
    const held = ev.wasHeld;
    ev.stable = k;
    ev.atEntry = telValues(ev.entry, read);
    ev.pre = telValues(ev.before, read); // until the next frames agree on better
    ev.preSeen = "";
    ev.wasHeld = ev.was ? ev.was(read) !== 0 : true;
    if (first || (!ev.change && k !== "1") || !held) {
        return;
    }
    telRecord(ev.name, telValues(ev.fields, read), ev.before.length ? pre : null,
              ev.entry.length ? entry : null);
}

// the start tap
function telemetryOpen() {
    if (!tel.config || tel.opened) {
        return;
    }
    tel.opened = true;
    telRecord("open", null, null);
}

function telSend(beacon) {
    if (!tel.on || tel.pending.length === 0) {
        return;
    }
    const recs = tel.pending.slice(0, TELEMETRY_POST);
    tel.seq++;
    const body = JSON.stringify({ game: tel.head.game, build: tel.head.build, where: tel.head.where,
                                  sid: tel.sid, load: tel.load, seq: tel.seq, recs });
    let sent = false;
    try {
        if (beacon && navigator.sendBeacon) {
            sent = navigator.sendBeacon(tel.config.endpoint, new Blob([body], { type: "text/plain" }));
        } else {
            fetch(tel.config.endpoint, { method: "POST", body, keepalive: true, mode: "cors",
                                         credentials: "omit",
                                         headers: { "Content-Type": "text/plain" } })
                .catch(() => {});
            sent = true;
        }
    } catch (err) {
        sent = false;
    }
    if (sent) {
        tel.pending.splice(0, recs.length);
    }
}

// the page hidden or closed: a quit with the state now, sent at once
function telQuit() {
    if (!tel.on || !tel.opened || tel.quitted) {
        return;
    }
    tel.quitted = true;
    let values = null;
    try {
        values = telValues(tel.quit, tel.read);
    } catch (err) {
        values = null;
    }
    telRecord("quit", values, null);
    telSend(true);
    telSend(true);
}

document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "hidden") {
        telQuit();
    } else if (tel.quitted) {
        tel.quitted = false;
        telRecord("back", null, null);
    }
});
window.addEventListener("pagehide", telQuit);
setInterval(() => telSend(false), TELEMETRY_FLUSH_MS);

// the bar's STATS button: whether it is on, and turning it off or on (kept in this browser);
// off drops what was waiting
function telemetryShown() {
    return { has: tel.config !== null, on: tel.on, blocked: tel.blocked };
}

function telemetrySet(on) {
    if (tel.blocked) {
        return false;
    }
    if (!on) {
        tel.pending = [];
    }
    if (on && !tel.on) {
        // the frames missed while off: what holds now is where it starts again, no events
        for (const ev of tel.events) {
            ev.stable = ev.seen = null;
        }
    }
    tel.on = on;
    telStore(localStorage, TELEMETRY_KEY, on ? "on" : "off");
    return tel.on;
}

if (typeof module !== "undefined") {
    module.exports = { tel, telemetrySetup, telemetryFrame, telemetryOpen, telemetrySet, telSend };
}
