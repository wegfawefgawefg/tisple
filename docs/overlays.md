# Overlays and the card's bank 1 (design note; steps 1 and 2 built)

Written 2026-10-30 for the owner to decide. Catwiz fills main memory and the language card;
the card's bank 1 (4 KB at `$D000-$DFFF`) is unused, and aux memory (46 KB) holds only data.
The game would like per-screen code and data loaded when the screen starts. This is what the
feature could look like in Tisple, what it costs, and how it meets the interrupt handler.
Step 1 of the recommendation below, whole functions in bank 1 behind stubs, is built: see
"Built: code in bank 1". Step 2, groups that take turns in one region, copied in from aux
memory, is built too: see "Built: overlay groups" at the end.

Names: bank 1 and bank 2 are Apple's. Bank 2 (`$C083`) is the one the startup switches in
and the link has filled since 2026-10-27; bank 1 (`$C08B`) is the second, new one.

## What the machine gives

- Bank 1 and bank 2 are two 4 KB RAMs at `$D000-$DFFF`; `$E000-$FFFF` (8 KB) is one RAM both
  share. Two reads of `$C08B` put bank 1 in for reads and writes, two of `$C083` bank 2 (what
  our startup does). `BIT` reads them without touching A, X or Y. `$C011` bit 7 says which bank
  is in.
- Aux memory is reached only with RAMRD and RAMWRT, which switch instruction fetches too, so
  code never runs from it: it must be copied to main memory first. lib/auxmem's copies do it
  (a `with_aux`, its code in both memories) with interrupts on (behind `auxmem.guard`), 16
  cycles a byte: 4 KB in about 66,000 cycles, 3.9 frames.
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

## Built: overlay groups (2026-11-01)

src/backend/overlay_guards.cpp (when code runs), overlay_plan.cpp and overlay_graph.cpp (which
groups), overlay.cpp (the groups in the code, the stubs and the copy), link_overlay.cpp (the
link's part); BuildOptions::use_overlays.

- **What a group is.** A set of roots and every function the program reaches only through
  them (from main, the handler or a function pointer), the barred ones left out: what the
  handler runs, main, a function whose address is taken (its entry and dispatcher too), an
  I/O base's patch function, code with an aux switch on or a with_aux. The roots are never
  copied into their callers (they go into BuildOptions::no_inline), so they keep their code;
  inside a group anything may be copied. The backend finds each group again in the generated
  code by its references (calls, tail jumps, branches, the program's calls for pointer calls),
  and leaves a group out when other code names one of its labels but by calling or jumping to
  a root's entry, when another group's code names it, when its code reaches another group's
  root through resident code, or when it has under 256 bytes.
- **When two groups take turns** (overlay_guards.hpp). The checker's program gives each
  function its facts: a global compared with constants (a `match` arm, `=` or `!=` with a
  constant, a bool global, `not`, `and`, `or`) as the set of values it holds or does not, in
  the code's order: a set of the global, or a call that may set it, forgets it; a loop forgets
  what its body sets; an `if` whose one branch returns leaves the other's facts after it
  (`(if (!= state.page none) (do ... (return)))`). A function's facts are what holds at all its
  calls. Two groups take turns when every root of one and every root of the other have facts
  that contradict (`page = 0` against `page in 1 2`), neither reaches the other's roots, and
  they share no code (what the two together reach is not more than an eighth of the smaller
  beyond what each reaches): roots that share code are parts of one screen, which change
  often, and go in one group.
- **Which groups, no syntax.** Only the facts a loop's own dispatch makes count (a `match` or
  `if` inside a `while` or `for`, in the function with the loop): Catwiz's main loop matches
  on `state.screen`, and a program whose loop does the same for its screens gets them as
  groups. The candidates are the functions with such facts and more than 24 expressions (a
  smaller one is copied into every caller anyway; the functions it calls take its place), the
  ones no other candidate reaches alone, each opened in turn for the ones inside it, and the
  sets of them that share code. The plan takes the set of them that frees the most (their sum
  less the biggest, which the region holds), each worth a load (64 expressions or more).
  Dispatches deeper in (Catwiz's `match state.phase` in battle.step) do not count: the compiler
  cannot tell a screen from the phases of a turn, which change every few frames, and a load
  at each would cost the battle's loop frames. That was the guess that went wrong: with every
  dispatch counted, Catwiz's plan put the dice's landing, the enemies' turn and the roll in
  three groups, a load at every phase change.
- **The fallback, `:overlay`.** `(fn step ((key u8)) bool :overlay ...)` says the function is a
  screen's entry. When any function is marked, only marked ones are roots: each with what
  only it reaches, and marked roots that cannot take turns (their facts do not contradict,
  one reaches the other) made one group, until every two can (since 2026-11-02 two marked
  roots that only share code stay two groups, the code they share resident: Catwiz's title and
  fight start share the art's loading); then the same
  choice of the set that frees the most. Every fact counts for marked roots, not only a
  loop's. A mark on a function that cannot be in a group (the handler's, main, a pointer's
  target, aux code) is a warning that says it stays resident.
- **When.** At the first level of squeeze the build without groups comes first; when it does
  not fit, the build with them (and with bank 1 too if it still does not). Past the first
  level the build with groups comes first, and only when it fits the one without (which
  would be faster): a compile a level. A group the code did not keep (too small, a rule
  broken) is dropped from the plan for the next levels, so its roots are copied again. A
  build with groups and bank 1 is still weighed against the tighter budgets without bank 1
  (with groups), as before. The build says what it did, a warning: the groups, their roots,
  functions, bytes and a load's cycles and frames, the stubs' and copy's bytes, the bytes
  freed. TISPLE_OVERLAY_NOTES=1 prints the plan's groups and their functions.
- **Where the images live: aux memory.** Only programs that already use aux memory (a let or
  const `:in aux`, a with_aux) get groups: the machine has it. The images go one after another
  next to what aux memory holds (so they join its segment: a boot disk holds 10, and Catwiz
  has 10), never in its text page or hi-res page 1 (80STORE). The copy is 16 cycles a byte:
  Catwiz's 2.8 KB page group loads in about 45,000 cycles, 2.7 frames. Not the disk: lib/disk
  reads 8.9 KB a second, 0.3 s for the same group, and Catwiz's aux memory has the room (2.7 KB
  left after both images); the owner's choice is the faster load. The cost of aux is at boot:
  the images load with the program (Catwiz's title 36 frames later, a track more).
- **The region.** The biggest group's bytes, after the resident code in the first stretch that
  holds it, the frames in memory after it; main memory or the card's bank 2 (Catwiz's is at
  $D99F). Not bank 1: it holds 4 KB either way, and with bank 1 already full of cold
  functions a region there would push as many bytes of them back. Each group's code is laid
  out at the region's address; the binary has its bytes only in its image.
- **What waits in the region at boot** (2026-11-03). A boot disk's program runs once
  (BuildOptions::runs_once), and no group is in the region before main starts, so the
  zero-page image, which only the startup reads, is laid out at the region's start and loads
  there with the program (RegionSpans::boot): its bytes cost nothing resident. Catwiz: 107
  bytes of the card. The startup's clearing of big lets `:in aux` with no value waits there
  too (link_overlay.hpp EmitAuxClear; docs/disks.md): a JSR of the startup's is all it
  costs resident. lib/auxmem's guard keeps its 108 bytes in aux memory the same way (the
  install reads them once with `auxmem.read`, interrupts off), not in the card.
- **Stubs.** `ov.r: JMP ov.r.load` for each root r, which the load points at r while its
  group is in: 3 cycles a call. The stubs are one block of JMPs, in group order.
  `ov.r.load: JSR ov.g.enter / JMP r`; `ov.g.enter: PHP / PHA / LDA #g / JMP ov.enter`. One
  `ov.enter` for every group (since 2026-11-03; it was one a group, about 75 bytes each): it
  pushes X and Y, copies the off template over the block (every stub at its load), loads g by
  its row of four small tables (its image in aux memory, its pages and bytes after, or its
  disk record), copies g's part of the on template (its stubs at their roots), and pulls them
  back. 9 bytes a root (stub, load, and the two templates' 6), 7 a group and 4 of tables,
  about 100 for ov.enter, plus the copy (51 bytes). At most 85 roots in all (the block is
  indexed by a byte); groups past that are left out. Catwiz: 590 bytes of stubs to 334.
- **Interrupts.** No handler code is in a group. The copy turns RAMRD on for a page at a time
  with interrupts off (PHP / SEI ... PLP): a tick waits at most a page's 4,100 cycles, and the
  handler never runs with RAMRD on, so no auxmem.guard is needed. The copy is the one piece
  of code that runs with RAMRD on: it goes with the mirrored code, in both memories. A tick
  in group code returns to it (the handler never touches the region).
- **Frames.** As before, by the call graph: groups that take turns never run together, so
  their frames share bytes.
- **Simulator.** Compiled::aux_switches are the copy's writes of $C003/$C002; the simulator
  loads the aux segments and, while the copy has RAMRD on, reads $0200-$BFFF from aux memory
  (its I/O hook widened), writes main memory. The program's own switches stay I/O to compare.
- **Tests.** tests/test_backend_overlay.cpp: three screens dispatched by the loop, and the
  same marked where the dispatch is not the loop's own (and unmarked: no groups), each built
  only because it does not fit, run on the simulator against the interpreter and on the
  emulated IIe; groups forced to take turns call after call; what stays out (the handler's
  code, a pointer's target, main); 300 ticks every 997 cycles on the emulated IIe while three
  groups load and run, all taken and counted right (162 landed in the copy, 21 in group
  code). The fuzzer's `(overlay N)` runs the fourth seed in four (its program and its
  (fnptr), (irq) and (stretch) ones) with groups of every function that may be in one (N = 1)
  or a seeded share, for even N with bank 1 functions too; `TISPLE_FUZZ_OVERLAY=n` reproduces
  one with `TISPLE_FUZZ_FILE`.
- **Catwiz** (2026-11-01, a copy of 195903b with seven marks: `title.step`, `title.draw`,
  `reward.start`, `reward.step`, `battle.ending`, `page.open`, `page.step`). Unmarked, the
  build is byte for byte main's: its loop's dispatch gives only the title, 91 bytes, too small.
  Marked: squeeze 8 instead of 10, two groups in a 2,795-byte region: the pages (page.open and
  page.step, the kit and rules pages, 16 functions, 2,795 bytes, a load 2.7 frames) and what
  comes between fights (battle.ending, reward.start, reward.step, 13 functions, 2,228 bytes,
  2.1 frames); the title's was dropped (91 bytes). 1,948 bytes of main memory and the card
  freed, 280 bytes of stubs and copy. A pass of the battle's loop flat out 5,721 cycles (was
  5,812, measure.py "900f:RETURN" 1500 1800); a pick shows in 2 frames (was 3); the title at frame 799 (was 763: the images load
  at boot, the disk a track longer); the fight ready 265 frames after RETURN (was 271); the
  kit page, its first open of a fight (a load), drawn whole in 61 frames as before, its first
  change 5 frames after B (was 3); the rules page as the first open 75 frames (was 77);
  closing a page 66 frames (was 68); the reward up at frame 1,211 (was 1,217, a fight won at
  once). Build 7.9 s (was 7.3 s).

## Built: images on the second disk (2026-11-02)

For Catwiz, whose aux memory is full: a group with a root marked `:overlay_disk2` keeps its
code on the second disk instead of in aux memory (BuildOptions::disk2_images, set for boot disk
builds; the program has tables `:on disk2` and imports lib/disk, else the group stays in aux).

- **Where.** Each such group's image is a disk table after the program's own on disk 2 (the
  next slots of the row, link_overlay.cpp PlaceGroupImages), in Compiled::overlays.disk_tables;
  `tisple build` writes them with the program's (WriteDisk2) and `--tables` lists them.
- **The load.** `ov.g.enter` jumps over the table's four-byte record (its bytes defined once the
  link has placed it), puts the record in auxmem.from and the region in auxmem.to, X the drive
  bit (`disk.disk2_bit`; main memory), and runs lib/disk's reader ($1700, then $1703 a sector)
  until it gives DONE; any error reads it again, since half a group's code cannot run. The
  reader holds interrupts off a sector at a time, as for any table. About 115 cycles a byte
  plus finding the first sector: Catwiz's 3 KB reward group about 0.4 s.
- **Tests.** tests/test_disk_read_two.cpp TestDiskOverlays: two groups marked
  `:overlay_disk2` and one `:overlay` (forced with overlay_test) taking turns every call on
  the emulated IIe with two drives, the sums as the plain code's, the images' places on disk 2.
- **Catwiz** (2026-11-02, the UI round's room pass): the reward group (3,057 bytes) and the
  fight's start (`battle.enter_fight` with its art's loading, 1,252 bytes, a Phase.start step
  of its own) from disk 2, the pages' group kept in aux memory so a page opens fast.

Not built: images on the boot disk, data only a
group reads in its image (Catwiz: 190 bytes), a region in bank 1, and a name on `:overlay`
to put marked roots in one group (the fight's phases as one group would need 10 KB of aux
memory Catwiz does not have).

## Built: data in bank 1, and code that switches the card (2026-11-04)

src/backend/card_data.cpp. For Catwiz's songs (main memory full, bank 1 with 1.8 KB free).

- **Lets `:in bank1`** (no value) take the bank's top, the first declared lowest; bank 1's code
  stretch ends below them, and the choice of bank 1 functions counts only the room they leave
  (LinkBank1: a candidate fits by its real bytes, and with lets there the three counts under the
  most are tried before the probes, since the most may not fit the shorter bank). The startup
  and the disk never write them; a program with them always links with the card.
- **Card switchers**: a function whose own code has an absolute `$C080-$C08F` operand (`$C080,X`
  is a slot's I/O and does not count), plus every function called from its code between a read
  of `$C088-$C08F` and the next of `$C080-$C087`, plus what those call (FindCardSwitching; calls
  of the runtime there put the runtime with them). That code goes right after the startup with
  the bank stubs and the interrupt entry (Bank1Resident checks they and the frames are out of
  `$D000-$DFFF`), is barred from bank 1 and overlay groups, and the variables it names are
  avoiders. Such a program, or one with lets in bank 1, has the banked interrupt entry even
  with no code in bank 1.
- Catwiz (2026-11-04): the songs' 1,790 bytes in bank 1, code there as before (15 functions,
  2,241 bytes, about 92,020 cycles a run); the handler, lib/disk's `read_card_with` (in
  `meter.read_as`) and `waiting.start` are the switched code.
