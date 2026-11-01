#!/bin/sh
# Reruns the compiler race and prints the table; arguments go to races/race.py
# (--kernels a,b  --compilers cc65,tisple  --jobs N  --variants  --update-doc).
# TISPLE_RACE_CPU=65c02 races on the 65C02 (races/README.md), into races/out/65c02/.
# Needs the competitors from races/install.sh and a built tisple (build/ or build-debug/,
# or TISPLE=path).
exec python3 "$(dirname "$0")/race.py" "$@"
