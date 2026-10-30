#!/usr/bin/env python3
"""Serves the phone player (web/dist) and the games published to it (web/publish.py).

    web/serve.py [--port 8650] [--home ~/.local/share/tisple/phone]

GET /             the player (web/dist)
GET /games/...    home/games: index.json, <name>/game.json and its disk
POST /log         a playtest's key log, saved as home/logs/<game>-<session>.json

Reach it from the phone over Tailscale: `tailscale serve --bg 8650` gives an https address,
which the browser needs for sound.
"""

import argparse
import json
import os
import re
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
DIST = os.path.join(HERE, "dist")


def make_handler(home):
    games = os.path.join(home, "games")
    logs = os.path.join(home, "logs")

    class Handler(SimpleHTTPRequestHandler):
        def translate_path(self, path):
            path = path.split("?", 1)[0].split("#", 1)[0]
            if path.startswith("/games/"):
                rel = os.path.normpath(path[len("/games/"):]).lstrip("/")
                if rel.startswith(".."):
                    return os.path.join(games, "missing")
                return os.path.join(games, rel)
            rel = os.path.normpath(path).lstrip("/")
            if rel.startswith(".."):
                return os.path.join(DIST, "missing")
            return os.path.join(DIST, rel)

        def end_headers(self):
            # every build is new: never let the phone keep an old one
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

        def do_POST(self):
            if self.path != "/log":
                self.send_error(404)
                return
            size = int(self.headers.get("Content-Length", "0"))
            if size <= 0 or size > 8 * 1024 * 1024:
                self.send_error(400)
                return
            try:
                log = json.loads(self.rfile.read(size))
            except ValueError:
                self.send_error(400)
                return
            game = re.sub(r"[^A-Za-z0-9_-]", "", str(log.get("game", "game")))[:40]
            session = re.sub(r"[^A-Za-z0-9_-]", "", str(log.get("session", "x")))[:40]
            os.makedirs(logs, exist_ok=True)
            with open(os.path.join(logs, f"{game}-{session}.json"), "w") as f:
                json.dump(log, f)
            self.send_response(204)
            self.end_headers()

        def log_message(self, format, *args):
            if self.command == "POST" or "/games/" in self.path:
                super().log_message(format, *args)

    return Handler


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8650)
    parser.add_argument("--bind", default="127.0.0.1", help="also the Tailscale address")
    parser.add_argument("--home", default=os.path.expanduser("~/.local/share/tisple/phone"))
    args = parser.parse_args()
    os.makedirs(os.path.join(args.home, "games"), exist_ok=True)
    server = ThreadingHTTPServer((args.bind, args.port), make_handler(args.home))
    print(f"phone player on http://{args.bind}:{args.port}/ (games in {args.home})")
    server.serve_forever()


if __name__ == "__main__":
    main()
