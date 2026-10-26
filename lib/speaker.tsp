; the speaker: (import speaker) then (speaker.click), (speaker.buzz half count)

(import apple2 a2)

; every access to SPKR moves the cone
(fn click () void
    (io_read u8 a2.SPKR))

; count clicks, half a period of delay loop apart: bigger half is lower
(fn buzz ((half u8) (count u8)) void
    (for i u8 0 count
        (click)
        (for d u8 0 half)))
