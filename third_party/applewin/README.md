# AppleWin (ported pieces)

From https://github.com/AppleWin/AppleWin, GPL-2.0-or-later (see `LICENSE`).
Commit: 9970ef6b668a7cf057ce6d82f100dde6a751f4d3

No files are vendored here; these parts were ported into our own files and
keep their attribution in the file headers:

- `src/machine/video_render.cpp`: the NTSC model from `source/NTSC.cpp`
  (Copyright 2010-2011 William S Simms, 2014-2016 Michael Pohoreski and Tom
  Charlesworth): the 4-phase chroma tables built by `initChromaPhaseTables`
  and its filters, the "color monitor ntsc" pixel path, the 14 MHz dot
  streams for text, lo-res and hi-res (bit 7 half-dot shift included), the
  two-dot start offset on colored lines, and the end-of-line pixels.
- `src/machine/video_scan.cpp`: `ScannerAddress` from
  `Video::VideoGetScannerAddress` in `source/Video.cpp` (after Sather's
  UTAIIe equations), and the color-burst counter rules from `NTSC.cpp`.
- `src/machine/bus.cpp`: paddle timing (2816 cycles full scale, from KEGS via
  `source/Joystick.cpp`) and the $C01x status/keyboard behaviour of
  `source/Memory.cpp`, read as a reference.

- `src/machine/disk2.cpp` (and the model described in `disk2.hpp`): the Disk II
  card after `source/Disk.cpp` (Copyright 1994-1996 Michael O'Brien, 1999-2001
  Oliver Schmidt, 2002-2005 Tom Charlesworth, 2006-2019 Tom Charlesworth,
  Michael Pohoreski and Nick Westgate): the stepper rule of `ControlStepperDeferred` (a magnet
  next to the head's own pulls it one half-track, no quarter tracks), motor
  off clearing the magnets, the read latch's 7-microsecond hold after a nibble
  extended while zeros follow (`DataLatchReadWOZ`), the head window's made-up
  bits after four zero cells (30% ones, after the WOZ notes), write protect
  saturating the latch (`LoadWriteProtect`), the one-second spin-down, and the
  deselected drive stopping. Our version keeps every bit cell (also in write
  mode) and loads the shift register at the cpu's write; the noise comes from
  a fixed LFSR so runs repeat.
- `src/disk/nibble.cpp` was checked against `source/DiskImageHelper.cpp`
  (`Code62`, `Decode62`, the sector order tables) for the 6-and-2 layout; the
  code is our own from Beneath Apple DOS.

- `src/machine/mouse.cpp`: the AppleMouse II card after `source/MouseInterface.cpp`
  (based on Kyle Kim's Apple in PC mousecard.cpp, reused in AppleWin with his
  permission): the 6805's side of the protocol (port B bits 4 and 5 strobing
  bytes, 6 and 7 answering, bits 1-3 paging the 2 KB rom into `$Cn00`), each
  command's byte count and answer, and the clamping. The 6821 follows
  `source/6821.cpp` (VICE's mc6821.c, Andreas Boose, GPL-2.0-or-later, with
  AppleWin's changes). Our CLAMPMOUSE takes signed bounds instead of AppleWin's
  Blazing Paddles fix, and HOMEMOUSE goes to the window's corner as Apple
  documents it.

Not used: AppleWin's disk boot code or firmware, and its copy of
`MouseInterface.rom`. Our mouse firmware (`src/machine/mouse_rom.cpp`) is
original; the owner's copy of Apple's is only read from
`~/.local/share/tisple/roms/` and never committed. Our disk ii boot rom
(`src/machine/disk2_rom.cpp`) and the self-booting disk's loader
(`src/disk/boot_loader.cpp`) are original.

Nothing from AppleWin's character set data (`NTSC_CharSet.cpp`) or its ROM
images is used: our text font is our own drawing (`src/machine/font.cpp`).
