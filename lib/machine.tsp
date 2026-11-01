; what Apple II the program runs on. (import machine) then (machine.detect) once, and
;   machine.model       machine.II, II_PLUS, IIE, IIE_ENHANCED, IIC, IIC_PLUS, IIGS, UNKNOWN
;   machine.kb          main memory and aux memory together: 48, 64 or 128
;   machine.aux         128K: aux memory, double res, 80 columns (lib/auxmem, text80, dhgr)
;   machine.col80       80-column hardware, aux memory or not (a IIe's 1K card)
;   machine.cmos        a 65C02 (enhanced IIe, IIc) or a IIgs's 65816; false: the nmos 6502
;   machine.mousetext   the alternate character set has MouseText (enhanced IIe, IIc, IIgs)
;   (machine.card n)    what is in slot n (1-7): machine.NONE, DISK2, STORAGE, MOUSE,
;                       MOCKINGBOARD, SERIAL, COL80 or OTHER
;   (machine.find kind) the first slot holding that card, 0 for none
; How: the rom's identification bytes and $FE1F (Apple's Miscellaneous Technical Notes #7 and
; #2), a decimal add the two chips flag differently, on a IIe the 80-column card's text page
; and a sparse-memory test of aux memory, and each slot's rom signature (the Mockingboard has
; none: its timer is watched). It borrows $0100-$0189, the bottom of the stack page, for our
; own 6502 code while it runs (RAMRD does not switch that page, and the language card can be
; switched out under it): the stack must stay above it, as it does unless some 50 calls deep.
; The language card is put back as it was, its ram with writes on as tisple's startup leaves
; it; on a II or II+ as bank 2.

(import apple2 a2)

(const UNKNOWN u8 0)
(const II u8 1)
(const II_PLUS u8 2)
(const IIE u8 3)
(const IIE_ENHANCED u8 4)
(const IIC u8 5)
(const IIC_PLUS u8 6)
(const IIGS u8 7) ; a IIgs in emulation: it runs this as an enhanced IIe

(const NONE u8 0)         ; nothing this knows: an empty slot or a card without a signature
(const DISK2 u8 1)        ; a Disk II controller (13 or 16 sectors)
(const STORAGE u8 2)      ; a ProDOS block device: hard disk or flash card, SmartPort
(const MOUSE u8 3)        ; an AppleMouse II (a IIc's slot 4)
(const MOCKINGBOARD u8 4) ; a 6522 timer that counts: a Mockingboard
(const SERIAL u8 5)       ; a serial card (Super Serial; a IIc's slots 1 and 2)
(const COL80 u8 6)        ; slot 3 of a IIe or IIc: the 80-column firmware
(const OTHER u8 7)        ; a card with Pascal 1.1 firmware of another kind

(let model u8 UNKNOWN)
(let kb u8 48)
(let aux bool false)
(let col80 bool false)
(let cmos bool false)
(let mousetext bool false)
(let slots (array u8 8))

; the bottom of the stack page, where the probes below run
(let low (array u8 138) :address 0x0100)

; our own 6502 code, copied to $0100 and run there, interrupts off.
; IDS: the language card's state kept ($C012, $C011), the rom switched in ($C082); the
; identification bytes $FBB3, $FBC0, $FBBF; SEC / JSR $FE1F (a IIgs clears the carry) and the
; flags kept; 99 + 1 in decimal mode (the nmos 6502 sets N from the binary $9A, the 65C02 from
; the decimal $00) and the flags kept; then the card back as it was: by the status on a IIe
; or IIc, and on an older machine its ram (bank 2) when $D000-$D007 read other than the rom
; did. Its answers at $017B on.
(const IDS_SIZE u8 138)

(const IDS (array u8 138)
    (make (array u8 138)
        0x08                                      ; $0100 ids: PHP
        0x78                                      ; $0101 SEI
        0xAD 0x12 0xC0                            ; $0102 LDA $C012
        0x8D 0x7B 0x01                            ; $0105 STA lc_ram
        0xAD 0x11 0xC0                            ; $0108 LDA $C011
        0x8D 0x7C 0x01                            ; $010B STA lc_bank
        0xA2 0x07                                 ; $010E LDX #7
        0xBD 0x00 0xD0                            ; $0110 keep: LDA $D000,X
        0x9D 0x82 0x01                            ; $0113 STA d000,X
        0xCA                                      ; $0116 DEX
        0x10 0xF7                                 ; $0117 BPL keep
        0xAD 0x82 0xC0                            ; $0119 LDA $C082
        0xAD 0xB3 0xFB                            ; $011C LDA $FBB3
        0x8D 0x7D 0x01                            ; $011F STA fbb3
        0xAD 0xC0 0xFB                            ; $0122 LDA $FBC0
        0x8D 0x7E 0x01                            ; $0125 STA fbc0
        0xAD 0xBF 0xFB                            ; $0128 LDA $FBBF
        0x8D 0x7F 0x01                            ; $012B STA fbbf
        0x38                                      ; $012E SEC
        0x20 0x1F 0xFE                            ; $012F JSR $FE1F
        0x08                                      ; $0132 PHP
        0x68                                      ; $0133 PLA
        0x8D 0x80 0x01                            ; $0134 STA gs_flags
        0xF8                                      ; $0137 SED
        0xA9 0x99                                 ; $0138 LDA #$99
        0x18                                      ; $013A CLC
        0x69 0x01                                 ; $013B ADC #$01
        0xD8                                      ; $013D CLD
        0x08                                      ; $013E PHP
        0x68                                      ; $013F PLA
        0x8D 0x81 0x01                            ; $0140 STA cpu_flags
        0xAD 0x7D 0x01                            ; $0143 LDA fbb3
        0xC9 0x06                                 ; $0146 CMP #$06
        0xD0 0x1A                                 ; $0148 BNE plus
        0x2C 0x7B 0x01                            ; $014A BIT lc_ram
        0x10 0x2A                                 ; $014D BPL done
        0x2C 0x7C 0x01                            ; $014F BIT lc_bank
        0x10 0x08                                 ; $0152 BPL bank1
        0xAD 0x83 0xC0                            ; $0154 LDA $C083
        0xAD 0x83 0xC0                            ; $0157 LDA $C083
        0x28                                      ; $015A PLP
        0x60                                      ; $015B RTS
        0xAD 0x8B 0xC0                            ; $015C bank1: LDA $C08B
        0xAD 0x8B 0xC0                            ; $015F LDA $C08B
        0x28                                      ; $0162 PLP
        0x60                                      ; $0163 RTS
        0xA2 0x07                                 ; $0164 plus: LDX #7
        0xBD 0x00 0xD0                            ; $0166 same: LDA $D000,X
        0xDD 0x82 0x01                            ; $0169 CMP d000,X
        0xD0 0x05                                 ; $016C BNE was_ram
        0xCA                                      ; $016E DEX
        0x10 0xF5                                 ; $016F BPL same
        0x30 0x06                                 ; $0171 BMI done
        0xAD 0x83 0xC0                            ; $0173 was_ram: LDA $C083
        0xAD 0x83 0xC0                            ; $0176 LDA $C083
        0x28                                      ; $0179 done: PLP
        0x60                                      ; $017A RTS
        0x00                                      ; $017B lc_ram: .byte 0
        0x00                                      ; $017C lc_bank: .byte 0
        0x00                                      ; $017D fbb3: .byte 0
        0x00                                      ; $017E fbc0: .byte 0
        0x00                                      ; $017F fbbf: .byte 0
        0x00                                      ; $0180 gs_flags: .byte 0
        0x00                                      ; $0181 cpu_flags: .byte 0
        0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00)) ; $0182 d000: .byte 0,0,0,0,0,0,0,0

(const LC_RAM u8 0x7B)
(const LC_BANK u8 0x7C)
(const FBB3 u8 0x7D)
(const FBC0 u8 0x7E)
(const FBBF u8 0x7F)
(const GS_FLAGS u8 0x80)
(const CPU_FLAGS u8 0x81)

; AUX_TEST, on a IIe with an 80-column card: RAMRD and RAMWRT on, $55 into aux memory's $0800
; and $AA into $0C00, what each reads back compared (a 1K card has the two in one byte), both
; bytes put back, the switches off. A is 0 for 128K.
(const AUX_SIZE u8 61)

(const AUX_TEST (array u8 61)
    (make (array u8 61)
        0x08           ; $0100 aux: PHP
        0x78           ; $0101 SEI
        0x8D 0x03 0xC0 ; $0102 STA $C003
        0x8D 0x05 0xC0 ; $0105 STA $C005
        0xAE 0x00 0x08 ; $0108 LDX $0800
        0xAC 0x00 0x0C ; $010B LDY $0C00
        0xA9 0x55      ; $010E LDA #$55
        0x8D 0x00 0x08 ; $0110 STA $0800
        0xA9 0xAA      ; $0113 LDA #$AA
        0x8D 0x00 0x0C ; $0115 STA $0C00
        0xAD 0x00 0x08 ; $0118 LDA $0800
        0x49 0x55      ; $011B EOR #$55
        0x8D 0x3C 0x01 ; $011D STA aux_seen
        0xAD 0x00 0x0C ; $0120 LDA $0C00
        0x49 0xAA      ; $0123 EOR #$AA
        0x0D 0x3C 0x01 ; $0125 ORA aux_seen
        0x8D 0x3C 0x01 ; $0128 STA aux_seen
        0x8C 0x00 0x0C ; $012B STY $0C00
        0x8E 0x00 0x08 ; $012E STX $0800
        0x8D 0x02 0xC0 ; $0131 STA $C002
        0x8D 0x04 0xC0 ; $0134 STA $C004
        0xAD 0x3C 0x01 ; $0137 LDA aux_seen
        0x28           ; $013A PLP
        0x60           ; $013B RTS
        0x00))         ; $013C aux_seen: .byte 0

(fn run_low ((code (ptr u8)) (size u8)) u8
    (for i u8 0 size
        (set (at low i) (load (+ code i))))
    (jsr 0x0100))

(fn decode ((fbb3 u8) (fbc0 u8) (fbbf u8) (gs_flags u8)) u8
    (if (= fbb3 0x38)
        (return II))
    (if (= fbb3 0xEA)
        (return II_PLUS))
    (if (!= fbb3 0x06)
        (return UNKNOWN))
    (if (= fbc0 0xEA)
        (return IIE))
    (if (= fbc0 0x00)
        (return (if (= fbbf 0x05)
            IIC_PLUS
            IIC)))
    (if (!= fbc0 0xE0)
        (return UNKNOWN))
    (if (= (& gs_flags 1) 0)
        (return IIGS))
    IIE_ENHANCED)

; the 80-column card's text page, through 80STORE and PAGE2 as lib/text80 writes it
(fn has_col80 () bool
    (io_write u8 a2.SET80STORE 0)
    (io_read u8 a2.HISCR)
    (let kept u8 (io_read u8 a2.TEXT_PAGE1))
    (io_write u8 a2.TEXT_PAGE1 0x88)
    (let first u8 (io_read u8 a2.TEXT_PAGE1))
    (io_write u8 a2.TEXT_PAGE1 0x77)
    (let second u8 (io_read u8 a2.TEXT_PAGE1))
    (io_write u8 a2.TEXT_PAGE1 kept)
    (io_read u8 a2.LOWSCR)
    (io_write u8 a2.CLR80STORE 0)
    (and (= first 0x88) (= second 0x77)))

(fn memory () void
    (if (or (= model IIC) (or (= model IIC_PLUS) (= model IIGS)))
        (do
            (set col80 true)
            (set aux true)
            (set kb 128)
            (return)))
    (if (or (= model IIE) (= model IIE_ENHANCED))
        (do
            (set kb 64)
            (set col80 (has_col80))
            (if col80
                (set aux (= (run_low (addr_of (at AUX_TEST 0)) AUX_SIZE) 0)))
            (if aux
                (set kb 128)))))

; a 6522's timer 1 low byte at page + 4: counting down, so two reads a few cycles apart differ
; by the same small number each time; then its latch at page + 6 keeps what is written
(fn timer_step ((t1 u16)) u8
    (let first u8 (io_read u8 t1))
    (let second u8 (io_read u8 t1))
    (- first second))

(fn is_mockingboard ((page u16)) bool
    (let t1 u16 (+ page 4))
    (let d u8 (timer_step t1))
    (if (or (< d 2) (> d 40))
        (return false))
    (for i u8 0 3
        (wait_cycles 97)
        (if (!= (timer_step t1) d)
            (return false)))
    (let latch u16 (+ page 6))
    (io_write u8 latch 0x55)
    (if (!= (io_read u8 latch) 0x55)
        (return false))
    (io_write u8 latch 0xAA)
    (= (io_read u8 latch) 0xAA))

; slot n's card by its rom: Disk II and ProDOS block devices answer $20 $00 $03 at $Cn01,
; $Cn03, $Cn05 (the boot signature), a Disk II with $3C at $Cn07; Pascal 1.1 firmware has $38
; $18 at $Cn05, $Cn07, $01 at $Cn0B and its kind at $Cn0C ($20 mouse, $31 serial, $88 80
; columns)
(fn classify ((n u8)) u8
    (let page u16 (+ 0xC000 (<< (as u16 n) 8)))
    (let b1 u8 (io_read u8 (+ page 0x01)))
    (let b3 u8 (io_read u8 (+ page 0x03)))
    (let b5 u8 (io_read u8 (+ page 0x05)))
    (let b7 u8 (io_read u8 (+ page 0x07)))
    (if (and (= b1 0x20) (and (= b3 0x00) (= b5 0x03)))
        (return (if (= b7 0x3C)
            DISK2
            STORAGE)))
    (if (and (= b5 0x38) (and (= b7 0x18) (= (io_read u8 (+ page 0x0B)) 0x01)))
        (do
            (let kind u8 (io_read u8 (+ page 0x0C)))
            (if (= kind 0x20)
                (return MOUSE))
            (if (= (& kind 0xF0) 0x30)
                (return SERIAL))
            (if (= kind 0x88)
                (return COL80))
            (return OTHER)))
    (if (is_mockingboard page)
        (return MOCKINGBOARD))
    NONE)

(fn scan_slots () void
    (let iie_or_iic bool (and (>= model IIE) (<= model IIGS)))
    (for n u8 1 8
        (if (and (= n 3) iie_or_iic)
            (set (at slots n) (if col80
                COL80
                NONE))
            (set (at slots n) (classify n)))
        (io_read u8 0xCFFF))) ; any card's $C800 rom off again

(fn detect () void
    (run_low (addr_of (at IDS 0)) IDS_SIZE)
    (let fbb3 u8 (at low FBB3))
    (set model (decode fbb3 (at low FBC0) (at low FBBF) (at low GS_FLAGS)))
    (set cmos (= (& (at low CPU_FLAGS) 0x80) 0))
    (set mousetext
        (or (= model IIE_ENHANCED) (or (= model IIC) (or (= model IIC_PLUS) (= model IIGS)))))
    (memory)
    (scan_slots))

(fn card ((n u8)) u8
    (at slots n))

(fn find ((kind u8)) u8
    (for n u8 1 8
        (if (= (at slots n) kind)
            (return n)))
    0)
