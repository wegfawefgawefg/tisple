; square-wave tones on the speaker: every access to $C030 flips the cone

(import apple2 a2)

(fn click () void
    (io_read u8 a2.SPKR))

; half_period sets the pitch: each pass of the empty loop costs the same cycles
(fn tone ((cycles u16) (half_period u8)) void
    (for i u16 0 cycles
        (click)
        (for j u8 0 half_period)))

(fn beep () void
    (tone 200 100))
