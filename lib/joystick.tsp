; the game port: a joystick (paddles 0 and 1, and a second one on 2 and 3), paddles, and the
; three buttons (on a IIe and IIc buttons 0 and 1 are also the open-apple and closed-apple
; keys). (import joystick) then
;   (joystick.read)            stick 0 into joystick.x and joystick.y (0-255, about 127 at the
;                              centre, 0 left and up), and joystick.button0 and button1
;   (joystick.read_pair 2)     paddles 2 and 3 (the second stick) into x and y, no buttons
;   (joystick.paddle n)        paddle n alone, 0-255, timed as the rom's PREAD and PDL are
;   (joystick.button n)        button n (0-2) is down: joystick.OPEN_APPLE, CLOSED_APPLE
;   (joystick.dir v)           -1, 0 or 1: v below LOW, between, or above HIGH (a digital stick)
; A read takes as long as the hardware's timers: about 11 cycles a step of the value, 2,900
; cycles (a sixth of a frame) at 255; read takes the longer of its two axes' times, so once a
; frame is cheap and twice a frame still fine. Interrupts wait while a paddle is timed (a
; Mockingboard tick up to 3 ms late). An unplugged paddle reads 255.
;
; Read both axes of a stick with read, not with two paddle calls: a paddle's timer that is
; still running when the next read starts the timers again is not restarted on a real
; machine, so the second call reads it short (Apple's IIe Technical Note #6).

(import apple2 a2)
(import tables)

(const OPEN_APPLE u8 0)
(const CLOSED_APPLE u8 1)

; the dead zone of dir
(const LOW u8 80)
(const HIGH u8 176)

(let x u8 127)
(let y u8 127)
(let button0 bool false)
(let button1 bool false)

; the timing loops, our own 6502 code, run where they lie: a let, since the pair's loop keeps
; its two counts in its last bytes (r0, r1), through STY operands aim fills in once.
;   pair (0): X = 0 or 2. Starts the timers and counts while paddles X and X+1 both run, 17
;     cycles a count (each paddle read at the same place in it); when one runs out, the other
;     alone, 17 a count again. r0 and r1 the counts, 255 for a timer that never runs out
;   one_paddle (88): X = 0 to 3. The rom's PREAD loop, 11 cycles a count; the count in A
(const PAIR u8 0)
(const ONE_PADDLE u8 88)
(const R0 u8 86)
(const R1 u8 87)
; the STY operands to aim: at r0, then at r1
(const R0_SITES (array u8 4) (make (array u8 4) 21 34 57 81))
(const R1_SITES (array u8 4) (make (array u8 4) 24 52 60 65))

(let code (array u8 109)
    (make (array u8 109)
        0x08           ; 0 pair: PHP
        0x78           ; 1 SEI
        0xAD 0x70 0xC0 ; 2 LDA $C070
        0xA0 0x00      ; 5 LDY #0
        0xBD 0x64 0xC0 ; 7 both: LDA $C064,X
        0x10 0x11      ; 10 BPL end0
        0xBD 0x65 0xC0 ; 12 LDA $C065,X
        0x10 0x30      ; 15 BPL end1
        0xC8           ; 17 INY
        0xD0 0xF3      ; 18 BNE both
        0x88           ; 20 DEY
        0x8C 0x56 0x00 ; 21 STY r0
        0x8C 0x57 0x00 ; 24 STY r1
        0x28           ; 27 PLP
        0x60           ; 28 RTS
        0xBD 0x65 0xC0 ; 29 end0: LDA $C065,X
        0x10 0x17      ; 32 BPL both_out
        0x8C 0x56 0x00 ; 34 STY r0
        0xC8           ; 37 INY
        0xEA           ; 38 NOP
        0xEA           ; 39 NOP
        0xBD 0x65 0xC0 ; 40 loop1: LDA $C065,X
        0x10 0x07      ; 43 BPL out1
        0xEA           ; 45 NOP
        0xEA           ; 46 NOP
        0xEA           ; 47 NOP
        0xC8           ; 48 INY
        0xD0 0xF5      ; 49 BNE loop1
        0x88           ; 51 DEY
        0x8C 0x57 0x00 ; 52 out1: STY r1
        0x28           ; 55 PLP
        0x60           ; 56 RTS
        0x8C 0x56 0x00 ; 57 both_out: STY r0
        0x8C 0x57 0x00 ; 60 STY r1
        0x28           ; 63 PLP
        0x60           ; 64 RTS
        0x8C 0x57 0x00 ; 65 end1: STY r1
        0xC8           ; 68 INY
        0xBD 0x64 0xC0 ; 69 loop0: LDA $C064,X
        0x10 0x07      ; 72 BPL out0
        0xEA           ; 74 NOP
        0xEA           ; 75 NOP
        0xEA           ; 76 NOP
        0xC8           ; 77 INY
        0xD0 0xF5      ; 78 BNE loop0
        0x88           ; 80 DEY
        0x8C 0x56 0x00 ; 81 out0: STY r0
        0x28           ; 84 PLP
        0x60           ; 85 RTS
        0x00           ; 86 r0: .byte 0
        0x00           ; 87 r1: .byte 0
        0x08           ; 88 one_paddle: PHP
        0x78           ; 89 SEI
        0xAD 0x70 0xC0 ; 90 LDA $C070
        0xA0 0x00      ; 93 LDY #0
        0xEA           ; 95 NOP
        0xEA           ; 96 NOP
        0xBD 0x64 0xC0 ; 97 ploop: LDA $C064,X
        0x10 0x04      ; 100 BPL pend
        0xC8           ; 102 INY
        0xD0 0xF8      ; 103 BNE ploop
        0x88           ; 105 DEY
        0x98           ; 106 pend: TYA
        0x28           ; 107 PLP
        0x60))         ; 108 RTS

(let aimed bool false)

; the STY at offset site stores to code's byte target
(fn aim_at ((site u8) (target u8)) void
    (let to u16 (as u16 (addr_of (at code target))))
    (set (at code (+ site 1)) (as u8 to))
    (set (at code (+ site 2)) (as u8 (>> to 8))))

(fn aim () void
    (for i u8 0 4
        (aim_at (at R0_SITES i) R0)
        (aim_at (at R1_SITES i) R1))
    (set aimed true))

; the pair loop's count, 17 cycles a step, as the 11-cycle steps of PREAD: 17 / 11 a count,
; 255 past the end
(macro scaled (r)
    (let v (round (/ (* r 17) 11)))
    (if (> v 255)
        255
        v))

(tables.table SCALE u8 256 scaled)

(fn read_pair ((first u8)) void
    (if (not aimed)
        (aim))
    (jsr (as u16 (addr_of (at code PAIR))) (x first))
    (set x (at SCALE (at code R0)))
    (set y (at SCALE (at code R1))))

(fn button ((n u8)) bool
    (>= (io_read u8 (+ a2.BUTN0 (as u16 n))) 0x80))

(fn read () void
    (read_pair 0)
    (set button0 (>= (io_read u8 a2.BUTN0) 0x80))
    (set button1 (>= (io_read u8 a2.BUTN1) 0x80)))

(fn paddle ((n u8)) u8
    (jsr (as u16 (addr_of (at code ONE_PADDLE))) (x n)))

(fn dir ((v u8)) i8
    (if (< v LOW)
        (return -1))
    (if (> v HIGH)
        (return 1))
    0)
