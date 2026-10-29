#!/bin/sh
# The random-program race; arguments go to races/random.py
# (--seed S --count N --jobs J --out DIR, --report, --one SEED; see races/README.md).
# Needs the competitors from races/install.sh and a built tisple (build/ or build-debug/,
# or TISPLE=path).
exec python3 "$(dirname "$0")/random.py" "$@"
