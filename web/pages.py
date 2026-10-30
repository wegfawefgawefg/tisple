#!/usr/bin/env python3
"""Builds the web demo: a static site (GitHub Pages) with the player and a few disks.

    web/pages.py OUT [--game NAME DISK CONFIG]... [--tisple build/tisple]

OUT gets the player (web/build.sh, so Emscripten is needed), a picker page, and a disk with
a screenshot for each demo in DEMOS, built from programs/ with its game.json beside it.
--game adds a disk built elsewhere with its player config (docs/phone.md), e.g. Catwiz:
    web/pages.py site --game catwiz ../catwiz/build/catwiz.dsk ../catwiz/tools/phone/catwiz.json
Added games come first in the picker. Only Tisple's own roms are used (the player has no
other), and web/rom_check.py looks over OUT at the end. OUT is replaced if a run of this
made it.
"""

import argparse
import hashlib
import html
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MARKER = ".tisple-pages"
REPO = "https://github.com/wegfawefgawefg/tisple"

# name, program folder, frames to run before the picker's screenshot
DEMOS = [
    ("plasma", "programs/plasma", 300),
    ("life", "programs/life", 300),
    ("snake", "programs/snake", 300),
    ("book", "programs/text_demo", 900),
]


def run(cmd):
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"failed: {' '.join(cmd)}\n{result.stdout}{result.stderr}")
    return result.stdout


def fresh_out(out):
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)):
            if os.listdir(out):
                sys.exit(f"{out} is not empty and was not made by web/pages.py")
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "games"))
    open(os.path.join(out, MARKER), "w").close()
    # GitHub Pages: serve the files as they are
    open(os.path.join(out, ".nojekyll"), "w").close()


def copy_player(out):
    dist = os.path.join(HERE, "dist")
    for name in os.listdir(dist):
        target = "play.html" if name == "index.html" else name
        shutil.copyfile(os.path.join(dist, name), os.path.join(out, target))


def add_game(out, name, disk, config, tisple, frames):
    with open(config) as f:
        game = json.load(f)
    with open(disk, "rb") as f:
        data = f.read()
    digest = hashlib.sha1(data).hexdigest()[:8]
    game["disk"] = "disk-" + digest + ".dsk"
    game["version"] = digest
    folder = os.path.join(out, "games", name)
    os.makedirs(folder)
    shutil.copyfile(disk, os.path.join(folder, game["disk"]))
    with open(os.path.join(folder, "game.json"), "w") as f:
        json.dump(game, f, indent=1)
    shot = os.path.join(folder, "shot.png")
    cmd = [tisple, "machine", "--disk", disk, "--frames", str(frames), "--screenshot", shot]
    if game.get("mockingboard"):
        cmd += ["--mockingboard", str(game["mockingboard"])]
    run(cmd)
    return game


def picker_html(entries):
    cards = []
    for name, game in entries:
        title = html.escape(game.get("title", name))
        about = html.escape(game.get("about", ""))
        cards.append(
            f'<a class="game" href="play.html?game={name}">'
            f'<img src="games/{name}/shot.png" alt="{title}" width="280" height="192">'
            f"<b>{title}</b><span>{about}</span></a>")
    return PICKER.replace("{cards}", "\n".join(cards)).replace("{repo}", REPO)


PICKER = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="theme-color" content="#0b0d0b">
<link rel="manifest" href="manifest.json">
<title>Tisple ][ demos</title>
<style>
:root { --bg: #0b0d0b; --panel: #161a16; --text: #d8e8d0; --dim: #7f907a; --accent: #7ee07a; }
* { box-sizing: border-box; }
body {
    margin: 0 auto; max-width: 960px; padding: 16px; background: var(--bg); color: var(--text);
    font: 15px/1.45 ui-monospace, "DejaVu Sans Mono", monospace;
}
h1 { font-size: 20px; margin: 0 0 4px; }
p { color: var(--dim); margin: 0 0 16px; }
a { color: var(--accent); }
#games { display: grid; gap: 12px; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); }
.game {
    display: flex; flex-direction: column; gap: 4px; padding: 10px; border-radius: 8px;
    background: var(--panel); color: var(--text); text-decoration: none;
}
.game img { width: 100%; height: auto; image-rendering: pixelated; background: #000; }
.game span { color: var(--dim); font-size: 13px; }
</style>
</head>
<body>
<h1>Tisple ][</h1>
<p>Disks built by the <a href="{repo}">Tisple ][</a> compiler, booting on its Apple IIe emulator
compiled to WebAssembly, with its own boot rom (no Apple rom). Tap a game, then tap the screen
to start. The on-screen buttons and a keyboard (arrows, return, letters) both work.</p>
<div id="games">
{cards}
</div>
</body>
</html>
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out")
    parser.add_argument("--game", nargs=3, action="append", default=[],
                        metavar=("NAME", "DISK", "CONFIG"))
    parser.add_argument("--tisple", default=os.path.join(ROOT, "build", "tisple"))
    parser.add_argument("--frames", type=int, default=1200,
                        help="frames before an added game's screenshot")
    args = parser.parse_args()
    out = os.path.abspath(args.out)
    tisple = os.path.abspath(args.tisple)
    if not os.path.exists(tisple):
        sys.exit(f"no {tisple}: cmake --preset release && cmake --build --preset release")

    print(run([os.path.join(HERE, "build.sh")]).strip(), flush=True)
    fresh_out(out)
    copy_player(out)

    entries = []
    for name, disk, config in args.game:
        entries.append((name, add_game(out, name, os.path.abspath(disk), os.path.abspath(config),
                                       tisple, args.frames)))
    with tempfile.TemporaryDirectory() as tmp:
        for name, folder, frames in DEMOS:
            disk = os.path.join(tmp, name + ".dsk")
            run([tisple, "build", os.path.join(folder, "main.tsp"), "--disk", disk])
            entries.append((name, add_game(out, name, disk, os.path.join(ROOT, folder, "game.json"),
                                           tisple, frames)))

    index = {"games": [name for name, _ in entries], "default": entries[0][0], "static": True}
    with open(os.path.join(out, "games", "index.json"), "w") as f:
        json.dump(index, f, indent=1)
    with open(os.path.join(out, "index.html"), "w") as f:
        f.write(picker_html(entries))

    check = subprocess.run([sys.executable, os.path.join(HERE, "rom_check.py"), out])
    if check.returncode != 0:
        sys.exit("rom check found something: look before publishing")
    print(f"{out}: {', '.join(index['games'])}")


if __name__ == "__main__":
    main()
