# Hardware libraries

The libraries for the Apple II's own hardware that came out of the survey in
docs/roadmap.md (section 5): the joystick and paddles, what machine a program runs on,
saving to the boot disk, and the video modes the other libraries left out. The IIe,
enhanced IIe and IIc are the targets; the IIgs is only recognised.

## Joystick and paddles

```scheme
(import joystick)

(fn main () void
    (while true
        (timing.frame)
        (joystick.read)                     ; stick 0: joystick.x, joystick.y, button0/1
        (if (= (joystick.dir joystick.x) -1)
            (move_left))
        (if joystick.button0                ; open-apple on a IIe, fire on a stick
            (fire))))
```

| Call | Does |
| --- | --- |
| `(joystick.read)` | paddles 0 and 1 into `joystick.x` and `joystick.y`, buttons 0 and 1 into `joystick.button0`/`button1` |
| `(joystick.read_pair 2)` | paddles 2 and 3 (a second stick) into `x` and `y`, no buttons |
| `(joystick.paddle n)` | paddle n alone, 0-255, timed like the ROM's `PREAD` (Applesoft's `PDL`) |
| `(joystick.button n)` | button n (0-2) is down; `joystick.OPEN_APPLE` is 0, `CLOSED_APPLE` 1 |
| `(joystick.dir v)` | -1 below `joystick.LOW` (80), 1 above `HIGH` (176), else 0: a stick as four directions |

- Values are PREAD's: 0 at the top or left, about 127 at the centre, 255 at the bottom or
  right; an unplugged paddle reads 255. Real joysticks differ (trim, worn pots), which is
  why `dir`'s dead zone is wide.
- What a read costs. The paddle timers run 2,816 cycles at full scale, about 11 a step, and
  a read waits for them: `read` takes the longer of its two axes' times (2,816 cycles more
  at 255 than at 0, a sixth of a frame), not their sum. Once a frame is cheap; twice is fine.
  A stick held up-left costs almost nothing.
- How. Our own 6502 code (in `lib/joystick.tsp`, a `let` it runs in place: no fixed
  address): one trigger of the four timers, then one loop that reads both paddles at the
  same place in each 17-cycle pass and counts while both run, then the one still running
  alone at the same pace. The count is scaled to PREAD's 11-cycle steps by a table. Against
  the emulator's timers the result is within 1 of the value set, for every value on either
  axis (tests/test_joystick.cpp).
- `read`, not two `paddle` calls. A paddle's timer that is still running when the next
  read starts them again is not restarted on a real machine, so a second `PREAD` straight
  after a first reads its paddle short (Apple's IIe Technical Note #6). `read` starts the
  timers once. The emulator restarts them on every trigger, as AppleWin and MAME do, so it
  would not show that mistake.
- Interrupts are off while the timers are counted (SEI, then the caller's flags back):
  a Mockingboard tick can come up to 3 ms late.
- The buttons are plain reads of `$C061`-`$C063`; on a IIe and IIc buttons 0 and 1 are also
  the open-apple and closed-apple keys.

### In the emulator

- `--joystick "C:X,Y,B;..."`: from cycle C (or `30f`, frames) paddles 0 and 1 read X and Y
  and the buttons are down by B's bits (1 button 0, 2 button 1, 4 button 2). Headless runs
  and tests script the stick this way.
- The window: a game controller's left stick is paddles 0 and 1, its south face button (A)
  button 0, east (B) button 1, west and north button 2 (src/machine/sdl_gamepad.cpp). The
  left and right Alt keys are buttons 0 and 1, as before.
