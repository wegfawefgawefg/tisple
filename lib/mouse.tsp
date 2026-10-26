; the AppleMouse II card (slot 4 on a IIe, built in on a IIc), through its firmware
; (import mouse) then (mouse.init) once and (mouse.read) to update mouse.x (0-279),
; mouse.y (0-191) and mouse.button, in polled mode: no interrupts. With no card, mouse.found
; is false and both do nothing.

(let found bool false)
(let x u16 0)
(let y u8 0)
(let button bool false)

; the card's slot n, and what its firmware takes in X ($Cn) and Y ($n0)
(let slot u8 0)
(let cn u8 0)
(let n0 u8 0)
(let read_entry u16 0) ; READMOUSE, found once

; offsets in the card's rom of the bytes giving each routine's place in the $Cn00 page
(const SET_MOUSE u8 0x12)
(const READ_MOUSE u8 0x14)
(const CLAMP_MOUSE u8 0x17)
(const HOME_MOUSE u8 0x18)
(const INIT_MOUSE u8 0x19)

; the screen holes the firmware answers in, one byte per slot. CLAMPMOUSE reads its bounds
; from the slot 0 bytes
(let x_low (array u8 8) :address 0x0478)
(let y_low (array u8 8) :address 0x04F8)
(let x_high (array u8 8) :address 0x0578)
(let y_high (array u8 8) :address 0x05F8)
(let status (array u8 8) :address 0x0778) ; bit 7: the button is down

(fn slot_rom ((n u8)) u16
    (+ 0xC000 (<< (as u16 n) 8)))

; the AppleMouse's signature in a slot's rom
(fn is_mouse ((rom u16)) bool
    (and (= (io_read u8 (+ rom 0x05)) 0x38)
        (and (= (io_read u8 (+ rom 0x07)) 0x18)
            (and (= (io_read u8 (+ rom 0x0B)) 0x01) (= (io_read u8 (+ rom 0x0C)) 0x20)))))

; slots 1-7, the first card that answers
(fn find () bool
    (for n u8 1 8
        (if (is_mouse (slot_rom n))
            (do
                (set slot n)
                (set cn (+ 0xC0 n))
                (set n0 (<< n 4))
                (return true))))
    false)

; the address of a firmware routine: the page plus the byte at offset
(fn entry ((offset u8)) u16
    (let rom u16 (slot_rom slot))
    (+ rom (as u16 (io_read u8 (+ rom (as u16 offset))))))

(fn call ((offset u8) (value u8)) void
    (jsr (entry offset) (a value) (x cn) (y n0)))

; one axis's window (0 for x, 1 for y), low and high bounds through the slot 0 holes
(fn clamp ((axis u8) (low u16) (high u16)) void
    (set (at x_low 0) (as u8 low))
    (set (at y_low 0) (as u8 high))
    (set (at x_high 0) (as u8 (>> low 8)))
    (set (at y_high 0) (as u8 (>> high 8)))
    (call CLAMP_MOUSE axis))

; find the card; the hi-res screen as the window, the mouse at its top left, polled mode
(fn init () void
    (set found (find))
    (if (not found)
        (return))
    (set read_entry (entry READ_MOUSE))
    (call INIT_MOUSE 0)
    (clamp 0 0 279)
    (clamp 1 0 191)
    (call HOME_MOUSE 0)
    (call SET_MOUSE 1))

(fn read () void
    (if (not found)
        (return))
    (jsr read_entry (x cn) (y n0))
    (set x (| (as u16 (at x_low slot)) (<< (as u16 (at x_high slot)) 8)))
    (set y (at y_low slot))
    (set button (!= (& (at status slot) 0x80) 0)))
