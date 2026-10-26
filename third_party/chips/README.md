# chips (vendored)

Files from https://github.com/floooh/chips, zlib license (see `LICENSE`):

- `m6502.h`      from `chips/m6502.h`
- `m6502dasm.h`  from `util/m6502dasm.h`

Commit: 9e88298ce56319953ac7a43213a1120359f7a3a6

Unmodified. Compiled once in `src/emu/chips_impl.cpp` (with warnings off for
that file only). To update, copy the two files from a newer commit, change the
hash here, and rerun `tisple_tests` with the SingleStepTests data
(`scripts/fetch_singlestep.sh`).
