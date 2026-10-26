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

Nothing from AppleWin's character set data (`NTSC_CharSet.cpp`) or its ROM
images is used: our text font is our own drawing (`src/machine/font.cpp`).
