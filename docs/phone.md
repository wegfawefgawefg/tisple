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
  presses, so the screen itself takes taps (Witchdog: the dice, the enemies, BARK, END).
- `tisple machine --window --taps <config>` reads the same `taps` for the desktop window:
  a left click on a box presses its key until the button comes up.
- `?autostart` runs without the tap and without sound, for headless checks.
