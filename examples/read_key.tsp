; wait for a key and return its ASCII code

(const KEYBOARD u16 0xC000)
(const KEY_STROBE u16 0xC010)

; bit 7 of KEYBOARD is set while a key is waiting
(fn key_ready
    (>= (io_read u8 KEYBOARD) 0x80))

(fn read_key
    (while (not (key_ready)))
    (let key (io_read u8 KEYBOARD))
    (io_read u8 KEY_STROBE) ; clear the strobe so the next read waits again
    (& key 0x7F))
