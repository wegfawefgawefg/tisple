#!/usr/bin/env python3
"""Puts a disk on the phone player: the next reload on the phone plays it.

    web/publish.py NAME DISK CONFIG [--disk2 DISK2] [--keep-default] [--home ~/.local/share/tisple/phone]

CONFIG is the game's player config, kept with the game (docs/phone.md), e.g.
    {"title": "Catwiz", "mockingboard": 4,
     "pad": [{"size": "big", "keys": [{"label": "END TURN", "key": "E", "look": "white"}]}],
     "taps": [{"x": 240, "y": 126, "w": 40, "h": 17, "key": "E"}]}
A key is a character, a number, or RETURN ESC SPACE LEFT RIGHT UP DOWN TAB DELETE.
The game becomes the default one the player opens (not with --keep-default). --disk2: a program with tables on a
second disk (tisple build writes it beside the first) plays with it in drive 2.
A pad key's "when" (and "live") conditions name the game's vars: their addresses come from
the build's DISK.vars.json (docs/phone.md, "The pad follows the game").
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import time

# a name in a pad condition: a game's var (state.phase, battle/reward.choices), an enum's member
# (state.Phase.player) or one of the config's "names"
NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_/]*(?:\.[A-Za-z_][A-Za-z0-9_/]*)*")


def fnv1a32(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def conditions(game):
    """every condition in the config: the names', each pad key's when and live, and the
    telemetry's fields and events (web/telemetry.js)"""
    out = list(game.get("names", {}).values())
    for row in game.get("pad", []):
        for key in row.get("keys", []):
            out += [key[f] for f in ("when", "live") if f in key]
    tel = game.get("telemetry", {})
    fields = tel.get("fields", {})
    used = list(tel.get("quit", []))
    for event in tel.get("events", []):
        out += [event[f] for f in ("when", "was") if f in event]
        change = event.get("change", [])
        out += change if isinstance(change, list) else [change]
        used += event.get("fields", []) + event.get("before", []) + event.get("entry", [])
    out += [fields.get(name, name) for name in dict.fromkeys(used)]
    return out


def add_state(game, disk_path, disk):
    """the game's vars and enums its pad conditions name, from the build's game.vars.json
    beside the disk (tisple build writes it): the player reads them from the running game. A
    missing or stale file, or a name it does not have, publishes without them: the pad then
    shows every key (but those marked "state_only")"""
    texts = conditions(game)
    if not texts:
        return
    path = os.path.splitext(disk_path)[0] + ".vars.json"
    if not os.path.exists(path):
        print(f"warning: no {path} (tisple build writes it): the pad shows every key", file=sys.stderr)
        return
    with open(path) as f:
        built = json.load(f)
    if built.get("disk_fnv") != fnv1a32(disk):
        print(f"warning: {path} is not from this disk's build: the pad shows every key",
              file=sys.stderr)
        return
    names = game.get("names", {})
    vars_, enums, unknown = {}, {}, []
    for text in texts:
        for name in NAME.findall(text):
            if name in names or name in ("true", "false"):
                continue
            if name in built["vars"]:
                vars_[name] = built["vars"][name]
            elif name in built["enums"]:
                enums[name] = built["enums"][name]
            else:
                unknown.append(name)
    if unknown:
        sys.exit("pad conditions name what the build does not have: " + ", ".join(sorted(set(unknown))))
    game["vars"] = vars_
    game["enums"] = enums
    game["check"] = built["check"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("name")
    parser.add_argument("disk")
    parser.add_argument("config")
    parser.add_argument("--disk2")
    parser.add_argument("--home", default=os.path.expanduser("~/.local/share/tisple/phone"))
    parser.add_argument("--keep-default", action="store_true",
                        help="the player still opens the default game it had (a test build)")
    args = parser.parse_args()

    with open(args.config) as f:
        game = json.load(f)
    with open(args.disk, "rb") as f:
        disk = f.read()
    digest = hashlib.sha1(disk).hexdigest()[:8]
    add_state(game, args.disk, disk)
    game["disk"] = "disk-" + digest + ".dsk"
    game["version"] = time.strftime("%m-%d %H:%M") + " " + digest

    folder = os.path.join(args.home, "games", args.name)
    os.makedirs(folder, exist_ok=True)
    shutil.copyfile(args.disk, os.path.join(folder, game["disk"]))
    if args.disk2:
        with open(args.disk2, "rb") as f:
            game["disk2"] = "disk2-" + hashlib.sha1(f.read()).hexdigest()[:8] + ".dsk"
        shutil.copyfile(args.disk2, os.path.join(folder, game["disk2"]))
    # the disks the game.json no longer names go
    for old in os.listdir(folder):
        if old.startswith("disk") and old.endswith(".dsk") and old not in (game["disk"],
                                                                          game.get("disk2")):
            os.remove(os.path.join(folder, old))
    with open(os.path.join(folder, "game.json"), "w") as f:
        json.dump(game, f, indent=1)
    # keeps the disk the log was made on, for replays
    keep = os.path.join(args.home, "disks")
    os.makedirs(keep, exist_ok=True)
    shutil.copyfile(args.disk, os.path.join(keep, digest + ".dsk"))
    if args.disk2:
        shutil.copyfile(args.disk2, os.path.join(keep, digest + "-2.dsk"))

    index_path = os.path.join(args.home, "games", "index.json")
    index = {"games": []}
    if os.path.exists(index_path):
        with open(index_path) as f:
            index = json.load(f)
    if args.name not in index["games"]:
        index["games"].append(args.name)
    if not args.keep_default or "default" not in index:
        index["default"] = args.name
    with open(index_path, "w") as f:
        json.dump(index, f, indent=1)
    print(f"{args.name}: build {game['version']}")


if __name__ == "__main__":
    main()
