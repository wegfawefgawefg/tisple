# Mockingboard

Music and sound effects on a Mockingboard: `lib/mockingboard`, its song format
(`lib/mockingboard_song`), and the card in the built-in IIe. Started
2026-10-30 for Catwiz (then called Witchdog).

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
- `--wav F.wav` saves the speaker and both sound chips in stereo, as the card
  is: the first AY to the left, the second to the right, the speaker (and the
  drives) in both. `--mono-sound` mixes all of it into one channel, as before
  stereo. The window plays them too (F4: stereo or mono).
- `--stereo-width W` (0-1, default 0.5) is how far apart the chips are, in
  `--wav`, the window and (at the default) the phone player: 1 is hard left and
  right, as the card's outputs are; 0 both in the middle (the owner,
  2026-11-04: hard-panned is too far, about half). Equal power: a chip's gains
  are cos and sin of (1 - W) * pi/4 into its own side and the other, so it is
  as loud at any width (0.5: 0.92 and 0.38). The mono mix is unchanged by it.

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
  handler can poll other devices too. `(mb.service_bends)` in its place also
  plays vibrato, glides, slides and detune (Pitch that moves, below).
- `(mb.play song)` starts a song from the beginning; `(mb.stop)` silences the
  music (an effect playing goes on).
- `(mb.effect sound voice priority)` plays an effect on voice 0-5 from the
  next tick. It takes that voice while it plays; the music on it goes on
  silently and comes back after. One effect plays at a time: a new one with a
  higher priority than the one playing cuts it off, any other waits in line
  and starts on the tick the one before it ends. The line holds 4; more are
  dropped. Give effects a voice the music leaves free, or the least missed
  one.
- `(mb.effect_playing)` is true while an effect plays or waits. A sound that
  only matters now is dropped rather than queued with
  `(if (not (mb.effect_playing)) (mb.effect ...))`.
- `(mb.fade 15)` takes the music down to silence, a volume step every 4 ticks
  (15 steps: 1 s), then stops it; `(mb.fade_in 13)` after `(mb.play song)`
  starts the song 13 steps down and brings it up to full, a step every 4 ticks;
  `(mb.faded)` is true once the fade is where it goes. Effects keep their own
  volume (Fades, below).
- `(mb.set_rate cycles)` changes the tick from the next one on.
- `(mb.off)` before main returns: timer and interrupts off, every voice quiet.
  Without it the card goes on interrupting into whatever runs next.
- `play`, `stop`, `effect` and `init` turn interrupts on (`CLI`) when they
  return.
- No card: `mb.found` is false, and `play`, `stop` and `effect` do nothing.
  `(mb.speaker_effect sound)` plays an effect on the speaker while main waits
  (square waves at the notes' pitches, noise as jittered clicks; no volume, no
  decay): `(if mb.found (mb.effect s 5 1) (mb.speaker_effect s))`. A program
  that never calls it has none of its code: 669 bytes and the 354-byte
  division routine in the demo, which does call it.

Voices 0-2 are the first AY's channels A-C (`$Cn00`, left), 3-5 the second's
(`$Cn80`, right). The noise period and the hardware envelope belong to a chip,
not a voice: the three voices of a chip share them, and the last to set them
wins.

## The song format

Text, turned into bytes while compiling by `tune.song` and `tune.effect`;
each makes a top-level let, an array of u8. Placement options go last and on
to the let: `(tune.song music ... :in aux)`, `(tune.effect zap "..." :align
256)`. The player reads songs and effects as code reads memory, so they must
be in main memory or the language card when they play; a song `:in aux` is
copied in first (below). Tokens between spaces; a tick is one interrupt.

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

`end` last plays a voice's patterns once (2026-11-03, for stingers): `(voice 0 sting end)`.
At its list's end the voice stops, quiet, and the tick does no more for it; once every voice
has ended the music is stopped, as `(mb.stop)` leaves it, and a fade is done at once (Fades).

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
| `$E6` k r d | vibrato: shift k (8 - depth; 0 off), phase step r (256 / speed), delay d |
| `$E7` b | bend: 0 none, `$10` + k glide, `$20` + k slide up, `$30` + k slide down (k = 8 - speed) |
| `$E8` lo hi | detune: a signed u16 added to each note's period |
| `$FF` | end of the pattern |

A song starts with six u16 offsets from its first byte, 0 for a silent voice,
each to its voice's order list: u16 pattern offsets, then `$0000` and the
offset of the entry to loop back to. The patterns follow. A pattern is at most
255 bytes and must hold a note, rest or tie (the macros check both). An effect
is one pattern.

## Pitch that moves: w, p, s, t

Added 2026-11-03 (lib/mockingboard_bends). Only a program whose handler calls
`(mb.service_bends)` in place of `(mb.service)` plays these; `service` reads
their bytes as other commands. A program calls one or the other.

| Token | Meaning |
| --- | --- |
| `w2:10` `w2:10:12` | vibrato: depth 1-4, speed 2-64, delay 0-255 (0 if left out); `w0` off |
| `p4` | glide: each note slides from the pitch before it to its own; speed 1-7; `p0` off |
| `s+4` `s-4` | slide: each note goes up or down from its own pitch while it sounds; speed 1-7; `s0` off |
| `t+2` `t-2` | detune: each note's period 1-64 more (lower) or less (higher); `t0` none |

- Steps scale with the note: the vibrato's amount is the note's period >> (8 -
  depth), so depth 1-4 is about 1/8, 1/4, 1/2 and 1 semitone each way on any
  note; a glide's or slide's step a tick is the period >> (8 - speed), at least
  1: speed 1 about 1/8 semitone a tick, each one up doubling it, 7 about a
  fifth. The step is the new note's, worked out once when it starts.
- Vibrato: a triangle of two heights, half the amount and the amount, a
  quarter of a wobble each, above the note then below. Speed is the ticks of
  one wobble (10 is 6 Hz); under 4 it is more a trill. Delay: the ticks of each
  note before it wobbles. It changes from the next note on.
- Glide: from what the voice played last (rests between are fine); the first
  note after `play`, or of an effect, has nothing to glide from. It takes as
  many ticks as the interval needs at that speed: `p5` a fourth in about 3
  ticks, `p3` about 14.
- Slide: on to the end of the periods (0 up, 4095 down) and it stays there:
  drum and timpani drops (`s-1` to `s-3`), lasers (`s-5` on a high note), sirens
  with `s+`. A tie goes on sliding. `p` and `s` are one setting: the last
  written holds.
- Detune: in period units, so about 3 cents at C3, 12 at A4, 24 at A5. The same
  line on two voices a unit apart beats a few to a dozen times a second, a
  chorus (lib's demo song a_waltz_vib in Catwiz has it). Keep it below the
  note's period on high notes.
- A voice keeps w, p or s and t from one pattern to the next, like its length;
  `play` and `effect` start each voice with none.
- Past 12 bits (a deep vibrato on C1) the vibrato is left off for that tick.

### What it costs

- A program that calls `service` has none of it. The player's `events` and
  `tick` are templates (`events_fn`, `tick_fn`) that service's copies make byte
  for byte as before; the bends make their own with more in them. The state
  (16 bytes a slot, 112 in all) is lets only the bends' functions name, and the
  link leaves out a let only unreached functions name (docs/language.md,
  Static data). Catwiz's disks come out byte for byte the same with the
  branch's player as without it (and 4 bytes smaller than before the link
  change, from lets of its own libraries' unused calls). One thing is left:
  the compiler gives every function a frame, reached or not, so a small
  program's frames take 6-20 bytes more memory (the demo's 20, its image 48
  bytes more for the layout around them); Catwiz's are shared and do not grow.
- A program that calls `service_bends`: about 930-970 bytes more (the bends'
  song in tests/test_mockingboard_bends.cpp: 3424 bytes with `service`, 4350
  with `service_bends`; 974 more where the program fills memory and the size
  budget tightens), of which 112 are the state. Catwiz does not have that room
  today.
- The tick (that test's numbers): every playing slot checks one flag a tick
  when nothing moves. The demo's song (no w, p, s or t) with `service_bends`:
  1178 cycles on average (1004 with `service`), median 863 (770), at most 6767
  (5653: its first tick, every voice starting a note). One voice alone: a note
  held 317 cycles a tick, the same with `w4:10` 604 (about 290 a wobbling or
  sliding voice); a note every tick 1025, with `w2:10 p3` set 1631 (about 600 a
  note start that works out steps: a 7-step shift loop, 16-bit work and the
  first write). The test song with all four on: 831 on average, 6455 at most.

### The smaller player: w, p and t

Added 2026-11-04 for Catwiz's rest song (a_waltz_vib), lib/mockingboard_bends_small:
`(mb.service_bends_small)` or `(mb.service_bends_small_bank1)` in the handler plays w, p and t
exactly as `service_bends` does, the same periods on the same ticks, and reads an `s` without
playing it (the note holds its pitch). tests/test_mockingboard_bends_small.cpp plays a song with
a_waltz_vib's tune doubled a unit apart, its late vibrato and its glide, every depth, speed and
glide speed and detunes both ways (720 ticks), and an effect with them, through both players and
compares every voice's period, volume and mixer tick for tick.

- What it leaves out of the bends: the slide (no goal at the ends of the periods), the detune
  as a word (t's -64 to 64 is its low byte, signed), one shift loop for both of a note's steps
  (one loop for each instead), the period's own register writes (`player.out`, which writes the
  same registers: the volume and mixer it also looks at have not changed), and the glide's two
  ways (its stride is signed, toward 0, and stops where off changes sign).
- Bytes: the test's song 4960 with `service_bends`, 4704 with `service_bends_small`. At the
  tightest size budget (squeeze 10) the handler's code is 1,513 bytes against the plain
  player's 786 (`service` 229, `events` 557) and the bends' 1,600: about 730 bytes more than the
  plain player, the bends' about 815. The lets are 105 bytes (the bends' 112).
- In Catwiz (2026-11-04) it does not fit either: main memory and the card had about 120 bytes
  free and it needs about 850 more of code and frames (the build's fit notes, at 17 functions
  in bank 1) and its lets. Built for the 65C02 (954 bytes smaller) with its lets moved by hand to
  free scratch, Catwiz fits with it at 17 functions in bank 1 (12 without it).

## Fades

Added 2026-11-03 for Catwiz's song changes: the song before fades out, the
next one fades in, and the game goes on meanwhile.

- The player keeps `hush`, 0-60, and the volume each music voice gets is its
  level less `hush / 4`, 0 at the least: a fade is 15 volume steps of about
  3 dB, 4 ticks each, so 1 s from full to silent. `(mb.fade to)` sets where
  `hush` goes (`to` 0-15 steps); every tick moves it 1 nearer, and every 4th
  tick of a fade, the one where `hush / 4` changes, writes every voice's volume
  again (fading out at `hush` 4n, fading in at 4n + 3; until 2026-11-04 a fade
  in wrote them 3 ticks after the step, and only notes and decays took it at
  once). A fade in right after `play` so has its first rewrite on the song's
  first tick, which writes every voice anyway: one heavy tick fewer, and in
  Catwiz's first fight load the 6-voice rewrite no longer lands on the address
  field of a sector it wants (it cost two turns of the disk, 24 frames).
- At 15 steps (`hush` 60) the music stops, as `(mb.stop)` does: a faded song
  costs the tick nothing more, which matters while a disk loads (a song playing
  on silently took the time between sectors from lib/disk's reader and made
  Catwiz's fight loads about 20 frames longer). `(mb.fade to)` below 15 is a
  duck that goes on playing.
- `(mb.fade_in from)` puts `hush` at `from` steps at once and fades up to 0:
  `(mb.play song) (mb.fade_in 13)` after a fade to 15. A song whose loudest
  voice is `v13` is silent at 13 steps, so starting the fade there skips 8
  silent ticks. `play` keeps the fade as it is; `init` resets it to none.
- Only the music is faded. The effect playing (on the voice it borrows) keeps
  the volume its pattern gives it, and so does a voice on the hardware
  envelope (`h`, volume 16), which has no level to lower: it plays as written
  until the fade's end stops it.
- With no music voice playing (every voice of a song with `end` has ended, or none
  started), `(mb.fade to)` puts `hush` at its goal at once (2026-11-03): a song after a
  finished stinger starts at the next look instead of after a second of fading silence.
  tests/test_mockingboard_fade.cpp: a song whose two voices end after 16 and 8 ticks goes
  quiet, the fade after it is done at once, the next song plays from the next tick.
- `mb.faded` is `hush` at its goal. A program that replaces a song waits for it
  after `(mb.fade 15)` before it copies the next song over the one playing
  (Songs in aux memory, below); Catwiz's main loop looks once a pass.

What it costs (tests/test_mockingboard_fade.cpp checks the steps, the effect,
the envelope's voice, the stop and the fade in): the tick's fade step and
`heard` (the level less the fade, in `out` and the decay's write) are in
every program that plays music, called or not: about 140 bytes in a small
program (3168 to 3310 bytes for test_mockingboard_line's card effects), 250 in
another (the bends' test song with `service`, 3424 to 3680, with
`service_bends` 4350 to 4544). The tick with no fade going costs about 125
cycles more on average (the demo: 1004 to 1132, median 770 to 893, most 5653
to 5852): the compare each tick and a call for every volume write. During a
fade every 4th tick also rewrites the six voices' registers. `fade`,
`fade_in` and `faded` are only in a program that calls them. In Catwiz the
fade cost about 120 bytes of code in all (its build, before and after: code
44479 to 44629 bytes, language card 12277 to 12270, its bank 1 3545 to 3675).

## Songs in aux memory, copies while it plays

The handler runs from main memory, and lib/auxmem's copies switch main memory
out while they run (a `with_aux`: RAMRD on to read aux memory, RAMWRT to write it): an
interrupt in a copy would fetch the handler's code from aux memory, or write
its variables there. So copies needed interrupts off, in pieces short enough
not to lose a tick. `(auxmem.guard)`, once after `mb.init`, makes them safe
with interrupts on:

- 108 bytes of our 6502 code in page 3 of both memories. Both `$3FE` vectors
  (the roms' way to the handler; with RAMRD on the rom reads aux memory's) and,
  when the program runs in the language card, the card's `$FFFE` point to it.
  With RAMRD and RAMWRT off it goes straight on to the handler's entry: 19
  cycles a tick more (the demo's average tick 1004 to 1023). With one on, it
  turns both off, has the entry's `RTI` come back to it, puts them back and
  returns to the copy: about 65 cycles more for a tick that lands in a copy.
- The enhanced IIe's rom puts main memory in on its own way to `$3FE`; the
  guard then sees both off.
- It costs the 108 bytes (a const table, so only in a program that calls
  guard), about 35 bytes of code, and page 3 to `$03AB` in main memory and all
  of it in aux memory.
- tests/test_mockingboard_aux.cpp: 8 KB copies each way with the demo's song
  ticking, through the card's vector and Apple's two IIe roms; about 25 ticks
  land in reads and 25 in writes, none is lost, every byte arrives. The same
  program without the guard stops after its first tick.
- With the guard, lib/unpack's card modes and lib/dhgr's runs let a waiting
  tick in while they work in the aux language card (2026-11-03, docs/dhgr.md,
  Ticks through the card), so a song plays on through a screen's load.

A song in aux memory is copied into main memory to play. The handler reads the
song playing, so the copy runs with interrupts off (or after `mb.stop`, or once
`(mb.fade 15)` is done):

```scheme
(tune.song title ... :in aux)
(let playing (array u8 400)) ; the largest song

(sei)
(auxmem.read (addr_of (at playing 0)) (addr_of (at title 0)) 400)
(mb.play (addr_of (at playing 0)))
```

The player cannot read aux memory itself (What is left).

## Songs in bank 1

Added 2026-11-04 for Catwiz, whose main memory was full (docs/language.md, Bank 1 data):

```scheme
(let room (array u8 1790) :in bank1)                       ; the card's bank 1, at its top
(fn irq () void :interrupt (mb.service_bank1))             ; or service_bends_bank1
(disk.read_card_with (addr_of (at room 0)) SONG disk.BANK1 step)  ; a song :on disk2 there
(mb.play_bank1 (addr_of (at room 0)))
```

- `play_bank1` is `play` with bank 1 in for the header's reads and every music voice marked
  (`player.from_bank1`). `service_bank1` (`tick_bank1`) reads a marked voice's events, the bytes
  up to its next note, with bank 1 in and bank 2 back after (`banked_fn`); the effect's slot is
  never marked, so effects are read with bank 2 in, wherever they are. `service` and
  `service_bends` have none of this code and play a bank 1 song wrong. After `play_bank1` a
  song played with `play` is read the same way: fine in main memory or `$E000` up, not in bank
  2's `$D000-$DFFF`.
- The events and what they call run with bank 1 in, so Tisple keeps them, and what they name,
  out of `$D000-$DFFF` (they are handler code: never in bank 1 or a group anyway), and the build
  has the interrupt entry that puts bank 2 in for the handler.
- Cost: a music event 15 cycles more (the mark's test and two switches), about 8 cycles a tick
  on average; the slot loop now counts down (2026-11-04), which saved about 14 a tick for every
  program. tests/test_bank1_data.cpp: the same song from main memory and from bank 1 gives the
  same voices tick for tick (an effect on top included); a tick from main memory 1000 cycles
  on average (6864 at most), from bank 1 1008 (6922); before the count down 1012 (6864). In
  Catwiz's first fight, over 300 frames, the handler took 1,124 cycles a tick, was 1,131.
- Nothing is copied to start a song: Catwiz's song start went from about 8,000 cycles with
  interrupts off to about 1,000.

## What a tick costs

Measured on the machine (`tests/test_mockingboard_lib.cpp`), from the cpu's
interrupt entry to the end of its `RTI`, through the card's ram vector, by
round: the player and compiler of dogfood 53, 55 and 56.

| Case | 53 | 55 | 56 | Of a 17030-cycle frame |
| --- | ---: | ---: | ---: | ---: |
| demo, median tick | 1137 | 800 | 770 | 4.5% |
| demo, average | 1403 | 1030 | 1004 | 5.9% |
| demo, the busiest tick after the first | 5382 | 4489 | 4495 | 26% |
| demo, the first tick (every voice reads its first pattern) | 7358 | 5647 | 5653 | 33% |
| every register of both chips changing every tick | 13779 | 10631 | 10853 | 64% |

In 56 the fade writes its volume without `reg`'s compare (a fade always
changes it), 32 cycles off the average; the effect line's check costs 6. The
last row's program starts an effect every 8000 cycles, each cutting off the
one before: main did the start before, the tick does it now (about 220
cycles).

Apple's IIe rom adds about 20 cycles a tick on its way to `$3FE`; the enhanced
IIe's adds more. Where a tick goes now:

- The entry and exit (A, X and Y saved), the 6522's flag read and answer:
  about 75 cycles.
- The effect line: about 6 cycles when nothing waits; starting the next
  effect about 220.
- Each of the seven slots: 20 cycles when it is not playing, about 35 to count a
  note down, about 55 when its note fades and about 120 when the fade writes.
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

The demo's code is 4904 bytes (4635 in 55, 5055 in 53), the handler's side
1120 (the tick with fade inlined 167, `send` 72, `out` 184, `events` 546,
`start_effect` 151), plus the 137-byte patch of `page` that init and the
startup call; zero page 89 bytes with the demo's own. Main's `effect` is 120
bytes, where it was 819 with the speaker's version inside it; the demo still
has that version (669 bytes, and the 354-byte division routine), since it
plays its effects on the speaker with no card.

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

- The player reading songs where they live, in aux memory: since 2026-10-31 a
  `with_aux` in the handler can read a pattern byte there (about 12 cycles more a
  byte for the switches), with no `jsr`; not done yet. Before, a byte read with
  RAMRD on had to run from page 3 (a copy in both memories, as lib/auxmem's
  mover did), so the handler would `jsr` to it, about 21 cycles more a
  pattern byte. But a `jsr` anywhere in the handler makes the checker count
  every global as shared with the handler (src/check/interrupts.cpp: the
  routine may change any let), and Catwiz's main code grew 330 bytes from
  that alone. It needs a way to say a routine changes no let, for instance a
  `jsr` option the handler check honours. Until then songs in aux memory are
  copied into a buffer the size of the largest.
- Stereo WAVs: both chips go to one channel now.
- The handler's cost. The player is compiled Tisple (there is no inline
  assembly). What is left is mostly the compiled code's own: the parser's index
  and each slot's loop counter live in zero page and are loaded again after
  every call (a register write is a `JSR`), and each register is compared
  before it is written, about 25 cycles even when it has not changed.
- The AY's volume table is a 3 dB curve, not a measured one.
- Frames for functions nothing reaches: the compiler still gives each one a
  frame, so the bends' unreached functions take 6-20 bytes of a small
  program's memory (Pitch that moves). Leaving them out would follow the lets'
  rule, in src/backend/frames.cpp.
- The bends in Catwiz: about 950 bytes it does not have; the smaller player (w, p and t, no s)
  about 850 and its lets (The smaller player, above).
