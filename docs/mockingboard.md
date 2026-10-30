# Mockingboard

Music and sound effects on a Mockingboard: `lib/mockingboard`, its song format
(`lib/mockingboard_song`), and the card in the built-in IIe. Started
2026-10-30 for Witchdog.

## Run it

```sh
tisple run examples/mockingboard_demo.tsp --window --mockingboard 4 \
    --rom ~/.local/share/tisple/roms/Apple2e.rom
tisple run examples/mockingboard_demo.tsp --mockingboard 4 \
    --rom ~/.local/share/tisple/roms/Apple2e.rom --frames 3600 --wav demo.wav
```

- `--mockingboard N` puts the card in slot N (1-7). Without it the machine is
  as before. Slot 4 is the usual one and the mouse card's: with the
  Mockingboard there, there is no mouse card (`--mouse` is then an error).
  Slot 6 is the Disk II's when a disk is in. On a IIe, slot 3 is the
  80-column firmware's: a card there is never seen.
- The IRQ goes through `$FFFE`, so the machine needs an Apple rom (`--rom`)
  unless the program runs in the language card (its startup writes the card's
  `$FFFE`). With no rom and no card ram, the first interrupt jumps to `$FFFF`
  and the run stops.
- `--wav F.wav` saves the speaker and both sound chips mixed to one channel
  (the card is stereo). The window plays them too.

## A program

```scheme
(import mockingboard mb)
(import mockingboard_song tune)

(tune.song music
    (pattern bass "l8 v11 k3 d2 - r a1 - c2 -")
    (pattern arp "l8 v10 k1 d4 a4 c5 e5")
    (voice 0 bass)
    (voice 1 arp))

(tune.effect zap "l1 v15 c7 g6 d6 a5")

(fn irq () void :interrupt
    (mb.service))

(fn main () void
    (mb.init)
    (mb.play (addr_of (at music 0)))
    ...
    (mb.effect (addr_of (at zap 0)) 5 1)
    ...
    (mb.off))
```

- `(mb.init)` finds the card, resets both chips and starts the tick: timer 1
  of the first 6522, free-running, an interrupt every 17030 cycles (one video
  frame, 59.9 a second). `mb.found` and `mb.slot` say what it found.
- The program's `:interrupt` handler must call `(mb.service)`. It reads the
  6522's flags first and does nothing if the timer did not interrupt, so the
  handler can poll other devices too.
- `(mb.play song)` starts a song from the beginning; `(mb.stop)` silences the
  music (an effect playing goes on).
- `(mb.effect sound voice priority)` plays an effect on voice 0-5. It takes
  that voice while it plays; the music on it goes on silently and comes back
  after. A new effect replaces the one playing only if its priority is at
  least as high. Give effects a voice the music leaves free, or the least
  missed one.
- `(mb.set_rate cycles)` changes the tick from the next one on.
- `(mb.off)` before main returns: timer and interrupts off, every voice quiet.
  Without it the card goes on interrupting into whatever runs next.
- `play`, `stop`, `effect` and `init` turn interrupts on (`CLI`) when they
  return.
- No card: `mb.found` is false, `play` and `stop` do nothing, and `effect`
  plays on the speaker while main waits (square waves at the notes' pitches,
  noise as jittered clicks; no volume, no decay).

Voices 0-2 are the first AY's channels A-C (`$Cn00`, left), 3-5 the second's
(`$Cn80`, right). The noise period and the hardware envelope belong to a chip,
not a voice: the three voices of a chip share them, and the last to set them
wins.

## The song format

Text, turned into bytes while compiling by `tune.song` and `tune.effect`;
each makes a top-level let, an array of u8. Tokens between spaces; a tick is
one interrupt.

| Token | Meaning |
| --- | --- |
| `c4` `c#4` ... `b8` | a note, C1 to B8 (sharps only), for the length |
| `r` | silence for the length |
| `-` | the note before goes on for the length (no new attack) |
| `l6` | the length of what follows, 1-64 ticks |
| `v12` | the volume, 0-15; the hardware envelope off |
| `k2` | decay: the level drops 1 every 2 ticks of a note; `k0` holds it |
| `m1` | mixer: `m1` tone, `m2` noise, `m3` both, `m0` neither |
| `n8` | the chip's noise period, 0-31 |
| `h14:600` | the chip's envelope, shape 14, period 600; notes after it follow the envelope and restart it |

A voice keeps its length, volume, decay and mixer from one pattern to the
next. `play` starts every voice at `l8 v12 k0 m1`, `effect` at `l1 v15 k0 m1`.
The notes are tuned to A4 = 440 Hz at the card's clock, the Apple's
1,020,484 Hz.

A song is patterns, then a `voice` line for each voice it uses:

```scheme
(tune.song name
    (pattern a "...")
    (pattern b "...")
    (voice 0 a)               ; a, a, a, ...
    (voice 3 intro loop b b c)) ; intro once, then b b c over and over
```

Each voice runs its own order list, so patterns of different lengths drift
against each other and line up again after their common multiple. That is
how the demo works: an arpeggio of 12 pulses on voice 1 and the same with a
rest, 13 pulses, on voice 3, a bell of 5, a shaker of 3, a bass of 16, a pad
of 64.

The bytes:

| Byte | Meaning |
| --- | --- |
| `$00`-`$5F` | note 0-95 (C1 + n semitones) |
| `$60` / `$61` | rest / tie |
| `$80`-`$BF` | length, byte - `$7F` ticks |
| `$C0`-`$CF` | volume |
| `$D0`-`$DF` | decay |
| `$E0`-`$E3` | mixer |
| `$E4` n | noise period |
| `$E5` s lo hi | envelope shape s, period lo + 256 hi |
| `$FF` | end of the pattern |

A song starts with six u16 offsets from its first byte, 0 for a silent voice,
each to its voice's order list: u16 pattern offsets, then `$0000` and the
offset of the entry to loop back to. The patterns follow. A pattern is at most
255 bytes and must hold a note, rest or tie (the macros check both). An effect
is one pattern.

## What a tick costs

Measured on the machine (`tests/test_mockingboard_lib.cpp`), from the cpu's
interrupt entry to the end of its `RTI`, through the card's ram vector. Before:
the player and compiler of round 53 (dogfood 53); after: dogfood 54.

| Case | Before | After | Of a 17030-cycle frame |
| --- | ---: | ---: | ---: |
| demo, median tick | 1137 | 800 | 4.7% |
| demo, average | 1403 | 1030 | 6.0% |
| demo, the busiest tick after the first | 5382 | 4489 | 26% |
| demo, the first tick (every voice reads its first pattern) | 7358 | 5647 | 33% |
| every register of both chips changing every tick | 13779 | 10631 | 62% |

Apple's IIe rom adds about 20 cycles a tick on its way to `$3FE`; the enhanced
IIe's adds more. Where a tick goes now:

- The entry and exit (A, X and Y saved), the 6522's flag read and answer:
  about 75 cycles.
- Each of the seven slots: 20 cycles when it is not playing, about 35 to count a
  note down, about 55 when its note fades.
- A register the player looks at but that has not changed: about 25 cycles (the
  compare in `reg`, inlined). One that changed: about 85, of which 38 are the
  six 6522 accesses themselves, 4 cycles each: `page` is an I/O base
  (docs/design.md), so `(io_write u8 (+ page 1) reg)` is a `STA $Cn01` whose
  operand init's `(set player.page page)` patched. Before, a write was a 14-arm
  match on the slot to reach fixed addresses (an `io_write` to a computed one
  costs 20 cycles where a fixed one costs 4): about 150 cycles.
- A voice starting a note: its pattern bytes, about 25 cycles each plus 5 for
  each kind of event the parser asks about before its own, then the note's
  period, level and mixer through `reg`: about 250-400 cycles. A new pattern
  from the order list adds about 100.

Ticks where many voices start notes at once cost the most, so a song whose
voices start on different ticks spreads its cost.

The demo's code is 4635 bytes (5055 before), the handler's side 940 (1451:
the tick with fade inlined 161, `send` 72, `out` 184, `events` 523), plus the
137-byte patch of `page` that init and the startup call; zero page 122 bytes
with the demo's own (129).

## The emulated card

`src/machine/mockingboard.hpp`, `via6522.hpp`, `ay8910.hpp`, written from the
chips' data sheets and the card's documented wiring, not from other emulators.

- The whole `$Cn00` page is the two 6522s: address bit 7 picks one, the low 4
  bits the register. Port A is the AY's data bus, port B bits 0-2 its BC1,
  BDIR and /RESET (BC2 high). Both IRQ outputs drive the cpu's IRQ, as a
  level: low while a flag is set and enabled, high when the program clears
  it.
- Timers: T1 and T2 count every cycle. Writing T1C-H at cycle w loads N; it
  reads N at w + 1, 0 at w + N + 1 and `$FFFF` at w + N + 2, when the flag
  sets; free-run reloads every N + 2. One-shot (and T2) count on down without
  another flag. They are kept as the cycle of their next underflow, so an
  idle card costs nothing between accesses.
- Not modelled: the shift register (it only keeps its byte), the CA/CB
  handshake lines, T2's pulse counting, the speech chip of the Sound/Speech
  card.
- The AY: tone counters at its clock / 8, noise (a 17-bit shift register) and
  envelope at clock / 16, the mixer, volumes on about 3 dB a step (the real
  chip's curve is close to that but not exact). The machine logs each register
  write with its cycle; the WAV and window replay the log, so the machine
  itself does no sound work.

Machine speed (release build, `tisple run ... --frames 3000`, three runs
each, before and after): plasma 232-239x real time before, 232-234x after;
Life 220-222x before, 217x after, 216-218x with an idle card in slot 5. About
1-2% for the IRQ line's compare each cycle. The demo with the card playing
runs at about 212x; with `--wav` the two AYs' synthesis brings it to about
150x.

## What is left

- Stereo WAVs: both chips go to one channel now.
- The handler's cost. The player is compiled Tisple (there is no inline
  assembly). What is left is mostly the compiled code's own: the parser's index
  and each slot's loop counter live in zero page and are loaded again after
  every call (a register write is a `JSR`), and each register is compared
  before it is written, about 25 cycles even when it has not changed.
- The AY's volume table is a 3 dB curve, not a measured one.
