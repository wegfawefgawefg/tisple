; aux memory, the extended 80-column card's second 64 KB: lets placed with :in aux, moved a
; block at a time. (import auxmem) then
;   (auxmem.read dst src count)   count bytes from aux memory at src to main memory at dst
;   (auxmem.write dst src count)  count bytes from main memory at src to aux memory at dst
;   (auxmem.guard)                once, in a program with an interrupt handler: from then on
;                                 read and write may run with interrupts on
; with pointers from addr_of: (auxmem.read (addr_of (at line 0)) (addr_of (at book i)) 80).
; Code reaches aux data only this way. With 80STORE on, $0400-$07FF (and $2000-$3FFF while
; HIRES is on) follow PAGE2 instead, so keep data out of the screens; :in aux does.
; (The file is not called aux.tsp: Windows keeps that name for a device.)

; the mover, our own 6502 code, written for this library. In: X = 0 to read aux (RAMRD on) or
; 2 to write it (RAMWRT on); from, to, pages and rest in zero page, which these lets keep from
; the rest of the program. While RAMRD is on the 6502 fetches its instructions from aux memory
; too, so the mover runs from page 3 ($0300), which nothing else uses, with a copy at the same
; address in aux memory: the first read or write puts both there.
(const HOME u16 0x0300)
(const MOVER_SIZE u8 42)
(let home_main (array u8 172) :address 0x0300 :reserve)        ; the mover, then the guard
(let home_aux (array u8 256) :address 0x0300 :in aux :reserve) ; and the vectors after it
(let pages u8 :address 0x00F9)                                 ; whole pages to copy
(let from (ptr u8) :address 0x00FA)                            ; moves on a page at a time
(let to (ptr u8) :address 0x00FC)
(let rest u8 :address 0x00FE) ; bytes after the whole pages

(const MOVER (array u8 42)
    (make (array u8 42)
        0x9D 0x03 0xC0                          ; STA $C003,X: RAMRD or RAMWRT on
        0xA0 0x00 0xA6 0xF9 0xF0 0x0E           ; LDY #0 / LDX pages / BEQ rest
        0xB1 0xFA 0x91 0xFC 0xC8 0xD0 0xF9      ; page: LDA (from),Y / STA (to),Y / INY / BNE page
        0xE6 0xFB 0xE6 0xFD 0xCA 0xD0 0xF2      ; INC from+1 / INC to+1 / DEX / BNE page
        0xA6 0xFE 0xF0 0x08                     ; rest: LDX rest / BEQ done
        0xB1 0xFA 0x91 0xFC 0xC8 0xCA 0xD0 0xF8 ; LDA (from),Y / STA (to),Y / INY / DEX / BNE
        0x8D 0x02 0xC0 0x8D 0x04 0xC0           ; done: STA $C002 / STA $C004: both off
        0x60))                                  ; RTS

; the guard, our own 6502 code too, in page 3 of both memories after the mover. Both $3FE
; vectors (the rom's way to the handler; with RAMRD on it reads aux memory's) and, when the
; program runs in the language card, the card's $FFFE come here. With RAMRD and RAMWRT off it
; jumps straight on to the handler's entry (16 cycles a tick more). With one on, the mover is
; running: it turns both off, has the entry return to it instead of to the mover (a return
; address and flags pushed for its RTI), puts them back and returns to the mover. Its install
; routine, run once with interrupts off, points its jump where $3FE points (the handler's
; entry), $3FE (and the card's $FFFE when the card's ram is in) to it, and copies $0340-$03FF
; to aux memory.
(const GUARD_HOME u16 0x0340)
(const GUARD_SIZE u8 108)
(const INSTALL u16 0x0373)

(const GUARD (array u8 108)
    (make (array u8 108)
        0x85 0x45                     ; STA $45: A where the rom's way keeps it
        0x2C 0x13 0xC0 0x30 0x05      ; guard: BIT $C013 / BMI fix: RAMRD on
        0x2C 0x14 0xC0 0x10 0x15      ; BIT $C014 / BPL go: RAMWRT off too
        0xAD 0x13 0xC0 0x48           ; fix: LDA $C013 / PHA
        0xAD 0x14 0xC0 0x48           ; LDA $C014 / PHA
        0x8D 0x02 0xC0                ; STA $C002: RAMRD off (main memory's copy runs on)
        0x8D 0x04 0xC0                ; STA $C004: RAMWRT off
        0xA9 0x03 0x48 0xA9 0x64      ; LDA #>back / PHA / LDA #<back
        0x48 0x08                     ; PHA / PHP: what the entry's RTI returns to
        0x4C 0x00 0x00                ; go: JMP entry
        0x68 0x10 0x03                ; back: PLA / BPL +3: RAMWRT was off
        0x8D 0x05 0xC0                ; STA $C005: on again
        0x68 0x10 0x03                ; PLA / BPL +3: RAMRD was off
        0x8D 0x03 0xC0                ; STA $C003: on again (aux memory's copy runs on)
        0xA5 0x45 0x40                ; LDA $45 / RTI
        0xAD 0xFE 0x03 0x8D 0x62 0x03 ; install: LDA $03FE / STA go+1
        0xAD 0xFF 0x03 0x8D 0x63 0x03 ; LDA $03FF / STA go+2
        0xA9 0x42 0x8D 0xFE 0x03      ; LDA #<guard / STA $03FE
        0xA9 0x03 0x8D 0xFF 0x03      ; LDA #>guard / STA $03FF
        0x2C 0x12 0xC0 0x10 0x0A      ; BIT $C012 / BPL aux: the rom in, not the card's ram
        0xA9 0x40 0x8D 0xFE 0xFF      ; LDA #<GUARD_HOME / STA $FFFE
        0xA9 0x03 0x8D 0xFF 0xFF      ; LDA #>GUARD_HOME / STA $FFFF
        0x8D 0x05 0xC0 0xA0 0x00      ; aux: STA $C005 / LDY #0: RAMWRT on
        0xB9 0x40 0x03 0x99 0x40 0x03 ; copy: LDA $0340,Y / STA $0340,Y
        0xC8 0xC0 0xC0 0xD0 0xF5      ; INY / CPY #192 / BNE copy
        0x8D 0x04 0xC0 0x60))         ; STA $C004 / RTS: RAMWRT off

(let ready bool false)
(let guarded bool false)

(const READ u8 0)
(const WRITE u8 2)

; the mover to page 3, then by itself to page 3 of aux memory
(fn setup () void
    (let home (ptr u8) (as (ptr u8) HOME))
    (for i u8 0 MOVER_SIZE
        (store (+ home i) (at MOVER i)))
    (set from home)
    (set to home)
    (set pages 0)
    (set rest MOVER_SIZE)
    (jsr HOME (x WRITE))
    (set ready true))

; count bytes from aux memory at src to main memory at dst
(fn read ((dst (ptr u8)) (src (ptr u8)) (count u16)) void
    (if (not ready)
        (setup))
    (set from src)
    (set to dst)
    (set pages (as u8 (>> count 8)))
    (set rest (as u8 count))
    (jsr HOME (x READ)))

; count bytes from main memory at src to aux memory at dst
(fn write ((dst (ptr u8)) (src (ptr u8)) (count u16)) void
    (if (not ready)
        (setup))
    (set from src)
    (set to dst)
    (set pages (as u8 (>> count 8)))
    (set rest (as u8 count))
    (jsr HOME (x WRITE)))

; the guard into page 3 of both memories and the interrupt vectors to it, once, after the
; startup has put the handler's entry in $3FE (anywhere in main). Turns interrupts on when it
; returns. Without it, read and write need interrupts off while a handler may run: the handler
; is in main memory, which RAMRD and RAMWRT switch out.
(fn guard () void
    (if guarded
        (return))
    (sei)
    (let home (ptr u8) (as (ptr u8) GUARD_HOME))
    (for i u8 0 GUARD_SIZE
        (store (+ home i) (at GUARD i)))
    (jsr INSTALL)
    (set guarded true)
    (cli))
