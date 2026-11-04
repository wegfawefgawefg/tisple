# Phone player

`tisple machine` built as WebAssembly (`web/`), for playtesting on a phone. It boots a
disk with our own roms (Apple's never leave the desktop), runs at the Apple's speed, plays
the speaker, a Mockingboard and the disk drive, and turns a game's on-screen buttons into
key presses.

- The bar's `disk` button turns the drive's sound on and off (dim when off; the browser
  remembers it; on until pressed: the owner, 2026-11-01, wants the real loading by default). On, a disk loads at the
  Apple's speed so it sounds like a Disk II: the boot's grind, the head stepping, the motor
  (docs/disks.md, "The drives' sound"); Catwiz takes about 11 s to load. Off, or with no
  sound running, loading runs flat out and silent as before.
- The sound is stereo, as the Mockingboard is: its first AY to the left, its second to the
  right, at the default width 0.5 (docs/mockingboard.md), the speaker and the drives in both
  (the owner, 2026-11-03: stereo by default, with a toggle). The bar's `stereo` button (at every width; it reads `mono`, dim, when mono)
  switches to the mono mix in both ears and back, at once, while the game plays too; the
  browser remembers it (`tisple.stereo`). Nothing about it on the start's tap (the owner,
  2026-11-03: not dead centre on the title).
- The volume (the owner, 2026-11-04): the bar's `-` and `+` in steps of 10%, the percent
  between them mutes (it reads `mute`, dim) and unmutes; the gain is the percent squared, so
  each step sounds about as big. Remembered (`tisple.volume`, `tisple.muted`). From 480 px
  across only: a phone held upright has its volume keys, and its bar no room.

- `web/build.sh`: needs Emscripten (`source <emsdk>/emsdk_env.sh`); builds `web/dist`.
- `web/publish.py NAME DISK CONFIG`: puts a disk in `~/.local/share/tisple/phone/games`
  with its button layout (CONFIG, kept with the game) and makes it the default. A reload
  on the phone plays it. Every published disk is also kept in `.../phone/disks/<sha1>.dsk`.
  `--disk2 DISK2`: a program with tables on a second disk (docs/disks.md, The second disk)
  plays with it in drive 2 (`"disk2"` in its game.json); `web/pages.py --game` takes the one
  beside DISK (`game2.dsk` for `game.dsk`) by itself.
- `web/serve.py --bind <tailscale ip>`: serves the page and the games (a second one on
  127.0.0.1 behind `tailscale serve --bg 8650` gives https://<machine>.<tailnet>.ts.net/). Over plain http
  the page plays sound through ScriptProcessor; over https (`tailscale serve`) through an
  AudioWorklet.
- Logs: the page records every key press and release with its machine cycle and posts
  them to `.../phone/logs/<game>-<session>.json` every 30 s and when it goes to the
  background, so a playtest can be replayed on the desktop against the disk it was made on.
- A game's config may set `"look": "ntsc"` (or `color`, `mono`): the screen look it was drawn
  for, which the player starts in; the look button still cycles.
- A game's config (`game.json`) names its buttons in `pad`: rows top to bottom, each
  `{"size": "small" | "medium" | "big", "keys": [...]}`, a key `{"label": "END TURN", "key":
  "E", "look": "white", "span": 2}` (looks: `card`, `white`, `violet`, `orange`, `green`;
  `\n` breaks a label in two; `sub` adds smaller words; the key letter shows small in a
  corner, or `hint`). Put the most pressed ones in the last rows, big: they are the ones in
  thumb reach. Small rows sit at the right. The older form, `buttons` in rows of `columns`
  with `sub` and `wide`, still works (the demos in `programs/` use it).
- The pad follows the game (2026-11-02, Catwiz): a key may have `"when"` (it shows only
  while that holds) and `"live"` (while it shows it can be pressed only while that holds,
  else it is dim and sends nothing), conditions over the running game's state, e.g.
  `"when": "state.phase == state.Phase.player && state.dice_n >= 5"`: numbers with `== != <
  <= > >= && || ! + -`, parentheses, `name[i]` for an element; `true`, `false`. A name is one
  of the config's `"names"` (named conditions, which may use each other), else a top-level let
  of the program (`state.phase`, `battle/reward.choices`; an array of structs by field,
  `state.actors.alive[1]`) or an enum's member (`state.Phase.player`). `"state_only": true`
  keeps a key hidden unless the state is read. A row with no key showing goes; the others
  share its width. Every `tisple build` (and `tisple run`) with a disk writes `game.vars.json`
  beside `game.dsk` (`src/disk/vars_export.hpp`: each let's address, bytes, element step,
  memory, the enums, the program's first bytes and an FNV-1a hash of the disk), and
  `publish.py` (and `pages.py`) copies the vars and enums the conditions name into the
  `game.json`, so the addresses are right for every build; a name the build lacks fails the
  publish, a missing or stale file publishes without them. The page reads them every frame with
  `tp_peek_ram` (main or aux ram whatever the switches say: `PeekMain`, `PeekAux`) once the
  program's first bytes are in memory and the disk has stopped after the boot; a change shows
  once two frames agree. Until then, or with no vars, every key shows but the `state_only` ones.
  The game spends no memory on it. A let the optimizer keeps no stores for (one only ever set
  to one constant) reads as its first value: name lets the game really changes.
  (`web/conditions.js`; `node` can load it alone for tests.)
- A game whose screen takes taps (`taps` or `mouse`) shows a line over the pad in portrait,
  "Turn sideways to play by touch" (or the config's `"sideways"` text; `""` for none): sideways
  the screen is as big as fits and the pad is gone.
- A config may list `taps`: boxes in hi-res pixels (280 x 192) with the key each presses,
  first match wins, so the screen itself takes taps and clicks.
- Or `"mouse": true` (Catwiz, 2026-11-01): the player puts a mouse card (our firmware) in slot
  4, and a tap moves its mouse there with the button down until the finger lifts; the game
  reads it with lib/mouse and knows what it drew where, so every box is exact in any layout,
  and a real Apple with a mouse plays the same. The Mockingboard then goes elsewhere (Catwiz:
  slot 5). Each press and lift is shown to the program's READMOUSE in turn, a press for 4
  frames at least (`PointMouse`, src/machine/mouse.hpp), so a game that reads the mouse once
  a pass of its loop misses no tap however long a pass takes. A tap shows a small flash where
  it landed. The log has taps as `[cycle, -2, x, y, down]`.
- Layout (`web/pad.js`): in portrait the screen goes across the top and the pad under it. In
  landscape, and on a desktop with a mouse, the screen is as big as fits; a game with `taps`
  gets no pad there, any other has it in a column beside the screen. The buttons and the bar
  letter in the machine's own font (`web/font.py` writes `font.js` from
  `src/machine/font.cpp` at build).
- A pad key may hold the joystick instead of a key: `{"label": "LEFT", "stick": "left"}`
  (`right`, `up`, `down`) holds paddle 0 or 1 at its end while pressed, back at 127 after;
  `{"label": "FIRE", "button": 0}` holds button 0 (open-apple; 1 closed-apple, 2). They are
  not in the playtest log, which replays keys.
- Saves (lib/save, docs/disks.md): what a program writes to its disk is kept in the browser's
  localStorage under the game's name, as the 256-byte blocks of the `.dsk` that differ from
  the published disk, once a second while the drive is off. At boot each kept block goes back
  where the published disk has all zeros (an empty save area), so saves outlive a new build
  that keeps its save areas where they were and never land on its program or other tables.
  Clearing the site's data clears them.
- `tisple machine --window --taps <config>` reads the same `taps` for the desktop window:
  a left click on a box presses its key until the button comes up; with `"mouse": true` a
  left click is the mouse card's mouse there, pressed, as a tap is on the phone.
- `?autostart` runs without the tap and without sound (so loading is flat out), for
  headless checks.

## Play stats

A game's config may ask for anonymous play records (`web/telemetry.js`, 2026-11-03, Catwiz):
`"telemetry": {"endpoint": URL, "fields": {...}, "events": [...], "quit": [...]}`, conditions
over the game's memory as the pad's (the same `names`, vars and enums; publish.py and pages.py
copy what they name), so a game opts in with config alone, no code and no memory.

- `fields`: a name for each value to report: a condition's number (`"state.actors.hp[0]"`,
  `"state.phase == state.Phase.won"`), or a var named alone with more than one element, read as
  the list (`"state.items"`). An event may also name a var directly.
- `events`: `{"name", "when"}` fires as `when` starts to hold (also when it holds at the first
  look); `{"name", "change": [conditions]}` whenever their values change (not at the first look),
  with `"was"`: only when that held just before. `"fields"` are read as it fires, `"before"` as
  they were just before (the frames before the change), `"entry"` as they were when the state it
  leaves began. A state counts once two frames agree, as the pad's.
- Built in: `open` (the start tap), `quit` (the page hidden or closed: the `quit` fields, sent at
  once with `sendBeacon`; a session's last one is where it ended), `back` (shown again).
- Each record: `e`, `t` (seconds since the page loaded), `dt` (since the record before), `gt`
  (the machine's seconds, which stop while the page is hidden), `v`, `pre`, `entry`. A post:
  `game`, `build` (the game.json version), `where` (`pages` or `phone`), `sid` (random, made
  once a tab session, kept in sessionStorage), `load` (page loads in that session), `seq`, `recs`.
  Posted every 20 s and as the page hides, as text/plain (a simple CORS request, no preflight),
  to an https endpoint (or http on 127.0.0.1 or localhost, for a test receiver). Nothing about
  the player: no address, browser or screen. Every failure is silent; at most 200 records wait.
- Off: the bar's `stats` button (for a game with telemetry, dim when off; only from 480 px across,
  the bar has no room on a phone held upright), or `?telemetry=off` (`=on` again); nothing on the
  start's tap (the owner, 2026-11-03: anonymous game-improvement data, no need to show it);
  kept in localStorage. Always off with Do Not Track or Global Privacy Control. `web/pages.py`
  puts a line under the picker naming the games that send them and how to stop them.
- Catwiz's receiver, report and events: its `tools/telemetry/` (README, "Play stats").
- `publish.py --keep-default` publishes a test build without making it the game the player opens.
  The phone server serves the player of the checkout `serve.py` runs from: a player change is
  live there once that checkout has it and `web/build.sh` ran.

## The web demo

`web/pages.py OUT [--game NAME DISK CONFIG]...` builds the same player as a static site for
GitHub Pages: a picker page (`index.html`, a screenshot of each disk made with
`tisple machine`), the player as `play.html`, and disks built from `programs/` with the
`game.json` kept beside each program. `--game` adds a disk built elsewhere with its config,
first in the picker; Catwiz: `--game catwiz <catwiz>/build/catwiz.dsk
<catwiz>/tools/phone/catwiz.json`. `games/index.json` says `"static": true`, so the page
keeps no log and shows a games button back to the picker. Only our roms are in it (the
player has no others); `web/rom_check.py OUT` then looks for byte runs of Apple's roms and
DOS and fails the build on a hit.

Publishing it to the `gh-pages` branch (Pages set to that branch, root):

```sh
web/pages.py /tmp/tisple-site --game catwiz ../catwiz/build/catwiz.dsk ../catwiz/tools/phone/catwiz.json
git worktree add --orphan -b gh-pages ../tisple-gh-pages   # once; later: git worktree add ../tisple-gh-pages gh-pages
rsync -a --delete --exclude .git --exclude .tisple-pages /tmp/tisple-site/ ../tisple-gh-pages/
git -C ../tisple-gh-pages add -A && git -C ../tisple-gh-pages commit -m "Web demo" && git -C ../tisple-gh-pages push origin gh-pages
```
