; square-wave tones on the speaker: every access to $C030 flips the cone

(import apple2 a2)

(fn click () void
    (io_read u8 a2.SPKR))

; half_period sets the pitch: the cycles waited after each click
(fn tone ((cycles u16) (half_period u16)) void
    (for i u16 0 cycles
        (click)
        (wait_cycles half_period)))

(fn beep () void
    (tone 200 808))
