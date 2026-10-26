; the keyboard: (import keys) then (keys.read), 0 when no key waits
; codes have bit 7 cleared, so they compare with apple2's KEY_ constants

(import apple2 a2)

; the waiting key, cleared so it reads once, or 0
(fn read () u8
    (let k u8 (io_read u8 a2.KBD))
    (if (< k 0x80)
        (return 0))
    (io_read u8 a2.KBDSTRB)
    (& k 0x7F))

; waits for a key
(fn wait () u8
    (while (< (io_read u8 a2.KBD) 0x80))
    (read))
