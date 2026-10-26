#!/bin/sh
# Reruns the compiler race and prints the table; arguments go to races/race.py
# (--kernels a,b  --compilers cc65,tisple  --jobs N  --variants  --update-doc).
# Needs the competitors from races/install.sh and a built tisple (build/ or build-debug/,
# or TISPLE=path).
exec python3 "$(dirname "$0")/race.py" "$@"
