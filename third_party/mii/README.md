# Mii (reference)

From https://github.com/buserror/mii_emu, MIT (see `LICENSE`), Copyright 2023
Michel Pollet.
Commit: cb27727bb2f8270fcb6545b3514d9f85b99f731c

No files are vendored here. `src/machine/bus.cpp` follows Mii's
`mii_access_soft_switches` (src/mii.c) for the language card's pre-write
state machine, the $C3xx / $CFFF internal-rom selection, and its keyboard
reads ($C000-$C01F returning the key in the low 7 bits); `src/machine/video_scan.cpp`
uses the same vertical-blank timing (12480 cycles drawn, 4550 blank). Mii's
ROM files and its character ROM are not used.
