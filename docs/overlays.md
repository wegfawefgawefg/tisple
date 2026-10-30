# Overlays and the card's bank 1 (design note; step 1 built)

Written 2026-10-30 for the owner to decide. Catwiz fills main memory and the language card;
the card's bank 1 (4 KB at `$D000-$DFFF`) is unused, and aux memory (46 KB) holds only data.
The game would like per-screen code and data loaded when the screen starts. This is what the
feature could look like in Tisple, what it costs, and how it meets the interrupt handler.
Step 1 of the recommendation below, whole functions in bank 1 behind stubs, is built: see
"Built: code in bank 1" at the end.

Names: bank 1 and bank 2 are Apple's. Bank 2 (`$C083`) is the one the startup switches in
and the link has filled since 2026-10-27; bank 1 (`$C08B`) is the second, new one.

## What the machine gives

- Bank 1 and bank 2 are two 4 KB RAMs at `$D000-$DFFF`; `$E000-$FFFF` (8 KB) is one RAM both
  share. Two reads of `$C08B` put bank 1 in for reads and writes, two of `$C083` bank 2 (what
  our startup does). `BIT` reads them without touching A, X or Y. `$C011` bit 7 says which bank
  is in.
- Aux memory is reached only with RAMRD and RAMWRT, which switch instruction fetches too, so
  code never runs from it: it must be copied to main memory first. lib/auxmem's mover does it
  from page 3 with interrupts on (behind `auxmem.guard`), about 10 cycles a byte: 4 KB in
  about 40,000 cycles, 2.4 frames.
- The disk: after boot the loader at `$0800-$0AFF` is gone. Reading a track needs a resident
  reader (about 400 bytes: seek, nibbles, decode) and takes two turns, 0.4 s for 4 KB.
  Built since for data (2026-10-30): lib/disk reads tables placed `:on disk`, its reader 1.2 KB
  at `$1700` with 768 bytes of tables below, 8.9 KB a second (docs/disks.md, Disk tables).

## The proposal: the link decides, no syntax

The compiler already knows the whole call graph, every function's bytes, what runs in the
handler, and (since the room round) when a program does not fit. So:

1. **Groups.** A group is a function the call graph enters from one place in a loop's
   branches (an arm of `(match state.screen ...)`, an `if` arm), with every function only it
   reaches, and the consts only that code reads. Catwiz's would be `title.step` with its
   pictures' drawing, `battle.step` with the rules, `reward.step`. Nothing the handler reaches
   is ever in a group (it runs between any two instructions, a copy included).
2. **When.** Only when the program does not fit after the size budget's squeeze (docs/design.md,
   Room), or as an earlier step of it: overlays cost load time, not cycles in the loop, so they
   may be the better trade before copies of hot code go. The build says what it did, as the
   squeeze does: "overlays: battle.step's group (6,980 bytes) and reward.step's (1,210) take
   turns in $D000-$DFFF; each change of screen copies one in from aux, about 2.5 frames".
3. **Where.** An overlay region: bank 1 (4 KB, free today), or a stretch of main memory the
   link keeps for it, sized to the biggest group. Each group's bytes live in aux memory (or on
   the disk, later), linked to run at the region's address.
4. **Calls into a group.** The root's callers call a stub in resident memory instead:
   `LDA #k; CMP current; BEQ go; JSR load_k; go: JMP root` (7 cycles when the group is in, the
   copy when not), with `BIT $C08B` twice before and `BIT $C083` twice after for bank 1 (about
   12 more cycles and 6 bytes; the stub then ends in `JSR root` and an RTS). Calls inside a
   group are plain JSRs. Code in a group may call resident code and read resident data freely;
   resident code never calls into a group but through a stub.
5. **Frames.** Groups that take turns never run at once, so their frames share bytes by the
   call graph as now.

An explicit form is the fallback if the guesses go wrong: `(fn step ...) :overlay` on the
root, or a module option. Not proposed first: the owner's principle is that the compiler
decides, and the call graph shows the screens.

## Costs

- Load: about 10 cycles a byte from aux. Catwiz's battle code is about 7 KB of the 18 KB
  (title 0.1 KB on its own, most of the title inlined into main), so a 4 KB bank 1 holds part
  of it; a main-memory region would take bytes from data. A screen change would take 2-4
  frames, done while the screen is blanked or before its first draw.
- Thrashing: if two groups alternate every frame (both called from one loop pass), each pass
  copies. The link cannot always see that; the rule "one group per arm of a branch in a loop"
  makes the common case right, and a wrong guess is slow, not wrong. A run-time count of loads
  (`--peek` on a counter the stub keeps) would show it.
- Bytes: about 16 bytes a stub (bank 1) or 12 (main), a 1-byte `current`, and a copy routine
  shared with lib/auxmem. Bank 1 alone, without aux: 4 KB of more code room for the price of
  the stubs and 20 cycles a call into it, with no loading at all, if the groups fit there.
- Compiler work: groups from the call graph (small), the link placing a group at the region's
  address while its bytes go to an aux segment (the link places code in stretches already;
  a group is a stretch with a different load address), the stubs, and the fuzzer running
  programs with groups through both paths. Bank 1 for data only (consts read by one group)
  is the smallest first step.

## The interrupt handler

- The handler and everything it calls, and every byte they read, stay resident: main memory
  or `$E000-$FFFF`. The link already knows that set (frames above every other function's,
  docs/design.md, Interrupt handlers).
- With bank 1 in use, `$D000-$DFFF` is not resident: today the link may put the handler's code
  there (bank 2). Either the link keeps handler code and data out of `$D000-$DFFF` (simplest:
  no cycles), or the IRQ entry reads `$C011` and puts bank 2 in and back (about 16 cycles a
  tick, 20 bytes). The vectors at `$FFFA-$FFFF` and `irq.direct` are in the shared 8 KB, so a
  tick taken while bank 1 is in reaches the entry either way.
- A tick during a copy from aux is what `auxmem.guard` handles now: the handler runs with
  main memory in and the copy goes on. Copying into bank 1 needs bank 1 in for writes, which
  the handler does not see if its code is resident.
- The enhanced IIe's ROM path (`$C3FA`) saves and restores the card's state around the
  handler, so a tick in a bank-1 function returns to bank 1; the card's own vector path
  (`irq.direct`) does not switch anything.

## Recommendation

1. First: the link may use bank 1 for whole functions no handler code reaches, called through
   stubs, when the program does not fit (4 KB, no loads, no thrashing; handler code kept out
   of `$D000-$DFFF`). That alone would have given Catwiz its 4 KB.
2. Then, if a game still needs room: groups loaded from aux into bank 1 or a main region.
3. Disk overlays only for something like chapters, where 0.4 s a track is fine.

## Built: code in bank 1 (2026-10-30)

src/backend/card_bank.hpp. No syntax; the link decides.

- **When.** A build at a level of squeeze links without the card, then with it, then with
  code in bank 1. So bank 1 comes before squeezing. A program that fits with bank 1 at level
  s is also built at the levels after s without it, until one fits or its cycles a run (each
  kept function's Gen::cost times its runs, plus 20 a crossing for the bank 1 build) pass the
  bank 1 build's; the faster is kept. The build says what it traded, a warning like the
  squeeze note: the functions, their bytes, the crossings and their cycles, and the squeeze
  that would fit without it.
- **Which functions.** A kept function may go unless an interrupt handler reaches it, it is
  a pointer call's dispatcher or entry (a function pointer is the entry's address, so those
  stay put and call through stubs), an I/O base's patch function, it has a JSR or JMP to an
  address or through one, an operand in `$D000-$DFFF`, or a label another function's code
  jumps into, reads, writes or takes the address of (or it does that to another's), but its
  entry, called. Candidates are ranked by crossings a byte, fewest first: calls into it
  (FindRunCounts), calls out of it (FindCallWeights, with the calls of functions copied
  everywhere handed to their callers), multiplies by their runs. The link takes the fewest
  of them that make the program fit (a search on the count), at most 4 KB with an eighth
  for far branches.
- **Stubs.** The rule is that bank 1 is in only while code in it runs. Every call or jump
  from anything else into bank 1 code goes through `bank1.f: BIT $C08B / JSR f / BIT $C083 /
  RTS`, every one out of it through `bank2.g: BIT $C083 / JSR g / BIT $C08B / RTS` (the
  runtime routines too). 10 bytes and 20 cycles a crossing over a plain JSR and RTS. One read
  switches: the startup's two reads of `$C083` enabled writes, and odd reads keep them on.
  BIT keeps A, X, Y and the carry; N, V and Z are never read after one of our calls. A tail
  jump through a stub is a call and a return, the same thing. Calls between bank 1 functions
  are plain JSRs, so nesting and recursion need nothing. The stubs and the interrupt entry
  go right after the startup, outside `$D000-$DFFF`. A function pointer call is
  dispatcher (main memory) to entry (main memory) to the function, through its stub when it
  is in bank 1; from bank 1 code the dispatcher is called through a back stub.
- **Data (decided).** Bank 2's `$D000-$DFFF` keeps holding data and code, as before; Catwiz
  has 4 KB of data there. Instead every variable the bank 1 functions' code names, and every
  variable whose address is taken when one of them reads or writes through a pointer, is
  kept out of `$D000-$DFFF` (LayoutOptions::avoiders), and so are the frames in memory. Bank 1
  holds code only. Code in bank 2's `$D000-$DFFF` is fine: it only runs with bank 2 in.
- **Interrupts.** No handler code goes to bank 1. The entry reads `$C011` (bit 7: bank 2 in),
  pushes it, switches bank 2 in, runs the handler, and switches bank 1 back after when it was
  in: 18 cycles more a tick with bank 2 in, 21 with bank 1. So the handler and its data may be
  anywhere but bank 1, `$D000-$DFFF` of bank 2 included. `$C011` needs a IIe (a II+ has no
  such status); a II+ with a 16K card and interrupts would need the handler kept out of
  `$D000-$DFFF` instead.
- **Disk and machine.** A bank 1 segment is `MemoryBank::kCardBank1` (`$D000-$DFFF`); the boot
  loader puts bank 1 in for writes (two reads of `$C08B`) before such a segment, the
  emulator's LoadSegment loads it there. The simulator keeps the bank not switched in aside
  and swaps on the build's own reads of `$C080-$C08F` (Compiled::card_switches), so the
  program's own I/O there is still compared.
- **Tests.** tests/test_backend_bank1.cpp: a program that does not fit main memory and the
  card gets its cold functions in bank 1 and runs as the interpreter does on the simulator
  and the emulated IIe; the stubs' bytes; handler, dispatcher and entries kept out; 200
  interrupts landing in bank 1 code and stubs with the handler reading a table in bank 2's
  `$D000-$DFFF`. Boot disks with bank 1 segments (tests/test_disk_segments.cpp). The fuzzer's
  `(bank1 N)` runs one seed in four (and its fnptr, irq and stretch programs) with every
  function that may go in bank 1 (N = 1) or a seeded half; odd N in 2.5 KB of main memory
  (`tight`), so data spills into `$D000-$DFFF` where bank 1 code must not look.
  `TISPLE_FUZZ_BANK1=n` and `TISPLE_FUZZ_TIGHT=1` reproduce one with `TISPLE_FUZZ_FILE`.
- **Catwiz** (2026-10-30): before, squeeze 3 (19,261 bytes of functions and runtime instead
  of 22,405, 26 functions called instead of copied, 19 loops rolled). Now squeeze 0 with 4
  functions in bank 1 (`title.enter`, `mockingboard.is_card`, `battle.step`,
  `turn.next_target`, 3,270 bytes, 3,321 with far branches). The battle's loop flat out went
  from 13,270 to 12,836 cycles a pass, the title's from 22,470 to 21,571 (tools/measure.py).
  The disk is one track longer (22 tracks), so the title is up by frame 750 instead of 700.

Not built: groups loaded from aux into bank 1 (step 2), disk overlays.
