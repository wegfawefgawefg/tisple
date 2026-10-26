; wait for a key and return its ASCII code

(import apple2 a2)

; bit 7 of KBD is set while a key is waiting
(fn key_ready () bool
    (>= (io_read u8 a2.KBD) 0x80))

(fn read_key () u8
    (while (not (key_ready)))
    (let key u8 (io_read u8 a2.KBD))
    (io_read u8 a2.KBDSTRB) ; clear the strobe so the next read waits again
    (& key 0x7F))
