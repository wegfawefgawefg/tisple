# Disks

Disk images, the emulated Disk II card, and disks that boot a Tisple program on
their own. Roadmap section 1, "Accurate and instant".

## Commands

```sh
tisple build main.tsp --disk game.dsk [--origin A] [-o game.bin]
tisple run main.tsp --disk [game.dsk] [machine flags]   # build, then boot the disk
tisple disk boot game.dsk game.bin [--origin A]         # any raw binary
tisple machine --disk a.dsk [--disk2 b.dsk] [--write-back]
tisple machine --disk a.dsk --rom Apple2e.rom [--disk-rom DISK2.rom]
tisple disk new|list|put|get|delete IMAGE ...           # DOS 3.3 files
```

- Images: `.dsk`/`.do` (DOS 3.3 sector order), `.po` (ProDOS order), both
  140 KB, and `.nib` (35 tracks of 6656 raw disk bytes). The DOS 3.3 file
  commands work on all three.
- `tisple machine --disk` boots with our boot ROM and no Apple ROM. With
  `--rom`, Apple's autostart finds the card in slot 6 and boots it, with our
  boot ROM or with `--disk-rom DISK2.rom`.
- `--save-snapshot` / `--load-snapshot` include the card and both disks:
  boot once, snapshot, restore. With `--load-snapshot`, the disks come from
  the snapshot and `--disk` only names the files `--write-back` writes.
- `--write-back` saves written disks to their files at the end, in the
  file's own format. A sector image is not saved if a track no longer reads
  as 16 good sectors (the error names the track and sector).

## The self-booting disk

- Track 0, physical sectors 0-1: our loader (`src/disk/boot_loader.cpp`,
  original, MIT under the runtime exception). Its first byte is 2, so the boot
  ROM loads both sectors to `$0800-$09FF` and jumps to `$0801`.
- Tracks 1 on: the program, 16 pages a track, from the page holding its
  origin. The rest of the disk is an empty DOS 3.3 volume with the program's
  tracks marked in use, so `tisple disk put` can add files.
- Size: the program must load in `$0A00-$BFFF` (46.5 KB, tracks 1-12). The
  loader runs in `$0800-$09FF`; no DOS sits at `$9600`, so the compile limit
  for `--disk` builds is `$C000`. A default `$6000` origin leaves 24 KB.
- The loader takes nothing from the boot ROM but the load of its two sectors
  and X = slot * 16. It turns the motor on, steps (2.5 ms per half-track each
  way, 25 ms settle) and reads with its own code, reading whichever wanted
  sector passes the head next: about two turns a track, 0.75 s for a one-track
  program in the emulator.
- At the jump: stack `$FF` (then the JSR), A = X = Y = 0, interrupts off,
  decimal off, language card `$C082` (as a direct load), motor off. It also
  used `$0296-$0365` and zero page `$DF-$EF`.
- If the program returns, the loader switches to text and enters the monitor
  (`$FF69`). The emulator stops there instead ("returned from the entry
  point"), like a direct load. A signature at `$09F0` ("TISPLE BOOT", a
  version, the return address) lets the machine find that address.

## Our boot ROM

`src/machine/disk2_rom.cpp`, used when no `DISK2.rom` is given. It is built
for the slot it sits in, uses no monitor routines and waits for nothing, so it
is only for the emulator. What a disk sees:

- `$Cn00`: cold boot. Motor on, drive 1, read mode; walks the head down 80
  half-tracks to track 0; reads physical sectors 0, 1, ... of track 0 to
  `$0800`, `$0900`, ... while the next sector number is below the byte at
  `$0800`; then `JMP $0801` with X = slot * 16.
- `$Cn5C`: read one more sector of the current track. In: `$26/$27` buffer,
  `$3D` physical sector, `$41` the track (compared with the address field),
  X = slot * 16 (not needed by ours). It reads, adds 1 to `$27` and `$3D`,
  reads again while `$3D` is below `($0800)`, else `JMP $0801` with X = slot *
  16. DOS 3.3's first boot sector uses this through `JMP ($003E)`.
- Zero page on entry to `$0801`: `$2B` = slot * 16, `$26` = 0, `$27` = the
  page after the last one read, `$3D` = the next sector, `$41` = 0, `$40` =
  the track found; `$3C` scratch; `$3E/$3F` never touched. A = `$3D`, Y = 0.
- `$0300-$0355` scratch; the nibble table at `$02D6 + n` (`$0356-$03D5`), as
  Apple's ROM leaves it.
- Signature: `$Cn01` = `$20`, `$Cn03` = `$00`, `$Cn05` = `$03`, `$Cn07` =
  `$3C` (the autostart ROM's test), `$CnFF` = `$00` (16 sectors, for ProDOS).

## The emulated card

`src/machine/disk2.cpp`, adapted from AppleWin (see
`third_party/applewin/README.md`). Tracks are loops of bit cells, 4 cycles
each: a disk byte takes 32 cycles, a self-sync byte 40, a track from a `.dsk`
51264 cells (205056 cycles, about 300 rpm). Every access catches the disk up
to the cycle, so copy protection that counts bits or reads half-finished bytes
sees what it would on a drive. Approximations are listed in
`src/machine/apple2.hpp`.

## Accuracy checks

- `tests/test_disk_boot.cpp` boots a self-booting disk with our ROM, with
  `Apple2e.rom` + our ROM, and with `Apple2e.rom` + `DISK2.rom`; the program's
  screen and memory match a direct load. The Apple ROM checks skip when
  `~/.local/share/tisple/roms/` lacks the files.
- a2audit (Zellyn Hunter, MIT): `scripts/fetch_a2audit.sh` puts
  `audit/audit.dsk` in `tests/data/a2audit/`; the tests then boot it with
  Apple's ROMs. Results (2026-10-26, version 1.07): language card tests and
  soft switch tests pass; it reports "main and aux are same: pretend 64K" and
  skips its aux memory test, since aux memory is not built yet. The video
  tests after its prompt are visual: run it in a window and compare by eye.

```sh
tisple machine --disk tests/data/a2audit/audit.dsk \
    --rom ~/.local/share/tisple/roms/Apple2e.rom \
    --disk-rom ~/.local/share/tisple/roms/DISK2.rom --window
```

## Real hardware

Not tried on a real machine yet. To try:

1. `tisple build main.tsp --disk game.dsk`.
2. FloppyEmu: copy `game.dsk` to its SD card, pick it in Disk II (5.25")
   mode, boot (`PR#6`, or power on with it in slot 6). Or ADTPro: send
   `game.dsk` to a real floppy over serial or audio, then boot that floppy.
3. Check: the program runs; returning from `main` lands at the monitor's `*`;
   `tisple disk list game.dsk` still reads the copy back from ADTPro.
4. What only real hardware tests: the loader's step timing (2.5 ms each way per
   half-track, 25 ms settle) on a real drive, and its read loops at a real
   drive's speed (within the usual 32-cycle budget, 22 cycles at most). A
   failure would show as a hang with the drive light on, likely while seeking.
   A slot other than 6 should work too (the loader is patched for the slot).
