# Overlays and the card's bank 1 (design note, not built)

Written 2026-10-30 for the owner to decide. Catwiz fills main memory and the language card;
the card's bank 1 (4 KB at `$D000-$DFFF`) is unused, and aux memory (46 KB) holds only data.
The game would like per-screen code and data loaded when the screen starts. This is what the
feature could look like in Tisple, what it costs, and how it meets the interrupt handler.

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
