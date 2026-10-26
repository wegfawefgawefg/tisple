; square-wave tones on the speaker: every access to $C030 flips the cone

(const SPEAKER u16 0xC030)

(fn click
    (io_read u8 SPEAKER))

; half_period sets the pitch: each pass of the empty loop costs the same cycles
(fn tone (args (cycles u16) (half_period u8))
    (for i 0 cycles
        (click)
        (for j 0 half_period)))

(fn beep
    (tone 200 100))
