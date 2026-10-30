# Phone player

`tisple machine` built as WebAssembly (`web/`), for playtesting on a phone. It boots a
disk with our own roms (Apple's never leave the desktop), runs at the Apple's speed with
the loading turbo'd, plays the speaker and a Mockingboard, and turns a game's on-screen
buttons into key presses.

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
- A game's config may list `taps`: boxes in hi-res pixels (280 x 192) with the key each
  presses, so the screen itself takes taps (Catwiz: the dice, reroll, the enemies, meow).
- `tisple machine --window --taps <config>` reads the same `taps` for the desktop window:
  a left click on a box presses its key until the button comes up.
- `?autostart` runs without the tap and without sound, for headless checks.

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
