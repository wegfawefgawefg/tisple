; a clock from the Mockingboard's timer: the 6522 in slot 4 counts down 17045 cycles (a 60th
; of a second) and raises an interrupt each time; the handler answers it and counts, and main
; shows the seconds until ten have gone

(import text40)

; slot 4's first 6522
(const T1_LOW u16 0xC404) ; reading it answers the interrupt
(const ACR u16 0xC40B)
(const IER u16 0xC40E)
(const PERIOD u16 17045)

(let ticks u16 0)

(fn tick () void :interrupt
    (io_read u8 T1_LOW)
    (set ticks (+ ticks 1)))

(fn start_timer () void
    (io_write u8 ACR 0x40)       ; timer 1 over and over
    (io_write u16 T1_LOW PERIOD) ; the high byte starts it
    (io_write u8 IER 0xC0))      ; timer 1's interrupt on

(fn show ((s u8)) void
    (text40.move 0 0)
    (text40.put (+ '0' (/ s 10)))
    (text40.put (+ '0' (% s 10))))

(fn main () void
    (text40.on)
    (start_timer)
    (let seconds u8 0)
    (show 0)
    (while (< seconds 10)
        ; ticks is two bytes: read it whole, with no interrupt between them
        (sei)
        (let now u16 ticks)
        (cli)
        (let s u8 (as u8 (/ now 60)))
        (if (!= s seconds)
            (do
                (set seconds s)
                (show seconds))))
    (io_write u8 IER 0x7F))
