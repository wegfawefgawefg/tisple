#!/usr/bin/env python3
"""Puts a disk on the phone player: the next reload on the phone plays it.

    web/publish.py NAME DISK CONFIG [--home ~/.local/share/tisple/phone]

CONFIG is the game's player config, kept with the game, e.g.
    {"title": "Catwiz", "mockingboard": 4, "columns": 5,
     "buttons": [{"label": "1", "key": "1"}, {"label": "go", "key": "RETURN", "wide": true}]}
A key is a character, a number, or RETURN ESC SPACE LEFT RIGHT UP DOWN TAB DELETE.
The game becomes the default one the player opens.
"""

import argparse
import hashlib
import json
import os
import shutil
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("name")
    parser.add_argument("disk")
    parser.add_argument("config")
    parser.add_argument("--home", default=os.path.expanduser("~/.local/share/tisple/phone"))
    args = parser.parse_args()

    with open(args.config) as f:
        game = json.load(f)
    with open(args.disk, "rb") as f:
        disk = f.read()
    digest = hashlib.sha1(disk).hexdigest()[:8]
    game["disk"] = "disk-" + digest + ".dsk"
    game["version"] = time.strftime("%m-%d %H:%M") + " " + digest

    folder = os.path.join(args.home, "games", args.name)
    os.makedirs(folder, exist_ok=True)
    shutil.copyfile(args.disk, os.path.join(folder, game["disk"]))
    # the disks the game.json no longer names go
    for old in os.listdir(folder):
        if old.startswith("disk-") and old != game["disk"]:
            os.remove(os.path.join(folder, old))
    with open(os.path.join(folder, "game.json"), "w") as f:
        json.dump(game, f, indent=1)
    # keeps the disk the log was made on, for replays
    keep = os.path.join(args.home, "disks")
    os.makedirs(keep, exist_ok=True)
    shutil.copyfile(args.disk, os.path.join(keep, digest + ".dsk"))

    index_path = os.path.join(args.home, "games", "index.json")
    index = {"games": []}
    if os.path.exists(index_path):
        with open(index_path) as f:
            index = json.load(f)
    if args.name not in index["games"]:
        index["games"].append(args.name)
    index["default"] = args.name
    with open(index_path, "w") as f:
        json.dump(index, f, indent=1)
    print(f"{args.name}: build {game['version']}")


if __name__ == "__main__":
    main()
