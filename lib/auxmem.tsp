; aux memory, the extended 80-column card's second 64 KB: lets placed with :in aux, moved a
; block at a time. (import auxmem) then
;   (auxmem.read dst src count)   count bytes from aux memory at src to main memory at dst
;   (auxmem.write dst src count)  count bytes from main memory at src to aux memory at dst
; with pointers from addr_of: (auxmem.read (addr_of (at line 0)) (addr_of (at book i)) 80).
; Code reaches aux data only this way. With 80STORE on, $0400-$07FF (and $2000-$3FFF while
; HIRES is on) follow PAGE2 instead, so keep data out of the screens; :in aux does.
; (The file is not called aux.tsp: Windows keeps that name for a device.)

; the mover, our own 6502 code, written for this library. It saves and restores the zero
; page it borrows ($F8-$FE), so it keeps the jsr rules. In: A = 0 to read aux (RAMRD on) or
; 2 to write it (RAMWRT on), X and Y = the low and high byte of args. It has no absolute
; addresses of its own. While RAMRD is on the 6502 fetches its instructions from aux memory
; too, so the mover runs from page 3 ($0300), which nothing else uses, with a copy at the
; same address in aux memory: the first read or write puts both there.
(const HOME u16 0x0300)
(let home_main (array u8 99) :address 0x0300 :reserve)
(let home_aux (array u8 99) :address 0x0300 :in aux :reserve)

(let mover (array u8 99)
    (make (array u8 99)
        0x48 0x8A 0x48 0x98 0x48                ; PHA TXA PHA TYA PHA: mode, args low, args high
        0xA2 0x06                               ; LDX #6
        0xB5 0xF8 0x48 0xCA 0x10 0xFA           ; save: LDA $F8,X / PHA / DEX / BPL save
        0xBA                                    ; TSX
        0xBD 0x09 0x01 0x85 0xF8                ; LDA $0109,X / STA $F8: args, low
        0xBD 0x08 0x01 0x85 0xF9                ; LDA $0108,X / STA $F9: and high
        0xA0 0x04                               ; LDY #4
        0xB1 0xF8 0x99 0xFA 0x00                ; copy: LDA ($F8),Y / STA $00FA,Y
        0x88 0x10 0xF8                          ; DEY / BPL copy: source, destination, count low
        0xA0 0x05 0xB1 0xF8 0x85 0xF9           ; LDY #5 / LDA ($F8),Y / STA $F9: whole pages
        0xBD 0x0A 0x01 0xAA                     ; LDA $010A,X / TAX: the mode
        0x9D 0x03 0xC0                          ; STA $C003,X: RAMRD or RAMWRT on
        0xA0 0x00 0xA6 0xF9 0xF0 0x0E           ; LDY #0 / LDX $F9 / BEQ rest
        0xB1 0xFA 0x91 0xFC 0xC8 0xD0 0xF9      ; page: LDA ($FA),Y / STA ($FC),Y / INY / BNE page
        0xE6 0xFB 0xE6 0xFD 0xCA 0xD0 0xF2      ; INC $FB / INC $FD / DEX / BNE page
        0xA6 0xFE 0xF0 0x08                     ; rest: LDX $FE / BEQ done
        0xB1 0xFA 0x91 0xFC 0xC8 0xCA 0xD0 0xF8 ; LDA ($FA),Y / STA ($FC),Y / INY / DEX / BNE
        0x8D 0x02 0xC0 0x8D 0x04 0xC0           ; done: STA $C002 / STA $C004: both off
        0xA2 0x00                               ; LDX #0
        0x68 0x95 0xF8 0xE8 0xE0 0x07 0xD0 0xF8 ; back: PLA / STA $F8,X / INX / CPX #7 / BNE
        0x68 0x68 0x68 0x60))                   ; PLA PLA PLA RTS

; source, destination and count, low bytes first, for the mover
(let args (array u8 6))
(let ready bool false)

(const READ u8 0)
(const WRITE u8 2)

(fn move ((mode u8) (dst (ptr u8)) (src (ptr u8)) (count u16)) void
    (let s u16 (as u16 src))
    (let d u16 (as u16 dst))
    (set (at args 0) (as u8 s))
    (set (at args 1) (as u8 (>> s 8)))
    (set (at args 2) (as u8 d))
    (set (at args 3) (as u8 (>> d 8)))
    (set (at args 4) (as u8 count))
    (set (at args 5) (as u8 (>> count 8)))
    (let block u16 (as u16 (addr_of args)))
    (jsr HOME (a mode) (x (as u8 block)) (y (as u8 (>> block 8)))))

; the mover to page 3, then by itself to page 3 of aux memory
(fn setup () void
    (let home (ptr u8) (as (ptr u8) HOME))
    (for i u8 0 99
        (store (+ home i) (at mover i)))
    (move WRITE home home 99)
    (set ready true))

; count bytes from aux memory at src to main memory at dst
(fn read ((dst (ptr u8)) (src (ptr u8)) (count u16)) void
    (if (not ready)
        (setup))
    (move READ dst src count))

; count bytes from main memory at src to aux memory at dst
(fn write ((dst (ptr u8)) (src (ptr u8)) (count u16)) void
    (if (not ready)
        (setup))
    (move WRITE dst src count))
