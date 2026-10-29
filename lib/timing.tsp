; frame timing on a IIe: (import timing) then (timing.frame), (timing.frames n); for exact
; delays in cycles, write (wait_cycles n)

(import apple2 a2)

; bit 7 of RDVBLBAR is 0 during vertical blank on a IIe: leave this blank, wait for the next
(fn frame () void
    (while (< (io_read u8 a2.RDVBLBAR) 0x80))
    (while (>= (io_read u8 a2.RDVBLBAR) 0x80)))

(fn frames ((n u8)) void
    (for i u8 0 n
        (frame)))
