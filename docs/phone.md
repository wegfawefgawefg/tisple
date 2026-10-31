# Phone player

`tisple machine` built as WebAssembly (`web/`), for playtesting on a phone. It boots a
disk with our own roms (Apple's never leave the desktop), runs at the Apple's speed, plays
the speaker, a Mockingboard and the disk drive, and turns a game's on-screen buttons into
key presses.

- The bar's `disk` button turns the drive's sound on and off (dim when off; the browser
  remembers it, default on). On, a disk loads at the Apple's speed so it sounds like a
  Disk II: the boot's grind, the head stepping, the motor (docs/disks.md, "The drives'
  sound"); Catwiz takes about 11 s to load. Off, or with no sound running, loading runs
  flat out and silent as before.

- `web/build.sh`: needs Emscripten (`source <emsdk>/emsdk_env.sh`); builds `web/dist`.
- `web/publish.py NAME DISK CONFIG`: puts a disk in `~/.local/share/tisple/phone/games`
  with its button layout (CONFIG, kept with the game) and makes it the default. A reload
  on the phone plays it. Every published disk is also kept in `.../phone/disks/<sha1>.dsk`.
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
- A config may list `taps`: boxes in hi-res pixels (280 x 192) with the key each presses,
  first match wins, so the screen itself takes taps and clicks (Catwiz: everything, with a
  last box over the whole screen for ESC).
- Layout (`web/pad.js`): in portrait the screen goes across the top and the pad under it. In
  landscape, and on a desktop with a mouse, the screen is as big as fits; a game with `taps`
  gets no pad there, any other has it in a column beside the screen. The buttons and the bar
  letter in the machine's own font (`web/font.py` writes `font.js` from
  `src/machine/font.cpp` at build).
- `tisple machine --window --taps <config>` reads the same `taps` for the desktop window:
  a left click on a box presses its key until the button comes up.
- `?autostart` runs without the tap and without sound (so loading is flat out), for
  headless checks.

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
