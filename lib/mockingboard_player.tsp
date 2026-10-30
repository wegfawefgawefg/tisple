; the interrupt side of lib/mockingboard: (mockingboard.service) in the program's :interrupt
; handler runs tick every timer interrupt. The state is plain globals: mockingboard.tsp's calls
; from main set them with interrupts off, and nothing here is called from main, so the
; handler has its own copy of every function it runs.
;
; Each tick: answer the 6522, then step the six music voices and the effect. A register is
; written when what a voice wants in it differs from what was written last; a write is six
; 6522 accesses at page plus a constant, which the compiler makes accesses to fixed addresses
; (it patches them when init sets page).

(import mockingboard_song tune)

(const VOICES u8 6)
(const FX u8 6) ; the effect's slot; slots 0-5 are the music's voices
(const SLOTS u8 7)

; the 6522s' registers, from the slot's page
(const ORB u16 0x00)
(const ORA u16 0x01)
(const DDRB u16 0x02)
(const DDRA u16 0x03)
(const T1C_L u16 0x04)
(const T1C_H u16 0x05)
(const T1L_L u16 0x06)
(const T1L_H u16 0x07)
(const ACR u16 0x0B)
(const IFR u16 0x0D)
(const IER u16 0x0E)
(const SECOND u16 0x80) ; the second 6522 and its chip

; port B: the AY's bus lines, /RESET high
(const BUS_RESET u8 0x00)
(const BUS_INACTIVE u8 0x04)
(const BUS_WRITE u8 0x06)
(const BUS_LATCH u8 0x07)

; the card, from mockingboard.init
(let found bool false)
(let page u16 0xC400) ; the card's page, $Cn00: its first 6522; the second at + SECOND

; what plays: the music's six voices and the effect, each a slot
(let song (ptr u8))
(let order (array (ptr u8) 6)) ; each voice's next order list entry
(let pos (array (ptr u8) 7))   ; each slot's pattern
(let index (array u8 7))       ; and the next byte in it
(let playing (array bool 7))
(let wait (array u8 7)) ; ticks left of the note
(let length (array u8 7))
(let volume (array u8 7)) ; 0-15, or 16 for the hardware envelope
(let level (array u8 7))  ; what sounds now
(let decay (array u8 7))
(let decay_left (array u8 7))
(let mix (array u8 7)) ; bit 0 tone, bit 1 noise
(let period (array u16 7))
(let fx_on bool false)
(let fx_voice u8 0)
(let fx_priority u8 0)
(let stale (array bool 6)) ; voices to work out again: their owner changed
(let any_stale bool false)
(let end_mark (array u8 1) (make (array u8 1) 0xFF)) ; where play points a voice first

; each slot's voice, as main sets it: the voice, its chip, whether the slot has it now (a
; music voice gives it to the effect while that plays), and where its tone and volume
; registers are in sent
(let voice (array u8 7))
(let chip_of (array u8 7))
(let owns (array bool 7))
(let tone_at (array u8 7))
(let volume_at (array u8 7))

; both chips' registers 0-13 (the second's from 14) as last written
(let sent (array u8 28))
(let voice_mix (array u8 6)) ; the mixer bits each voice's owner wants

(const CHIP_BASE (array u8 2) (make (array u8 2) 0 14))
(const VOICE_CHIP (array u8 6) (make (array u8 6) 0 0 0 1 1 1))
(const VOICE_TONE (array u8 6) (make (array u8 6) 0 2 4 14 16 18))
(const VOICE_VOLUME (array u8 6) (make (array u8 6) 8 9 10 22 23 24))
; each voice's chip's mixer register in sent, and the voice's two bits in it (tone, noise)
(const VOICE_MIXER (array u8 6) (make (array u8 6) 7 7 7 21 21 21))
(const VOICE_MIX_BITS (array u8 6) (make (array u8 6) 0x09 0x12 0x24 0x09 0x12 0x24))
; the mixer bits a voice's m (bit 0 tone, bit 1 noise) turns off (1 is off), for every channel
(const MIX_OFF (array u8 4) (make (array u8 4) 0x3F 0x38 0x07 0x00))

; one register write at a 6522's base address: the number latched, then the value written,
; each through the inactive state between
(macro write_at (via)
    `(do
        (io_write u8 (+ ,via ORA) reg)
        (io_write u8 ,via BUS_LATCH)
        (io_write u8 ,via BUS_INACTIVE)
        (io_write u8 (+ ,via ORA) value)
        (io_write u8 ,via BUS_WRITE)
        (io_write u8 ,via BUS_INACTIVE)))

; the handler's register write, the register at i in sent (0-13 the first chip's, 14-27 the
; second's): page plus a constant, so 4 cycles an access (a computed address takes 20)
(fn send ((i u8) (value u8)) void
    (set (at sent i) value)
    (if (< i 14)
        (do
            (let reg u8 i)
            (write_at page))
        (do
            (let reg u8 (- i 14))
            (write_at (+ page SECOND)))))

; main's, for the few writes it makes
(macro psg_writer (name)
    `(fn ,name
        ((chip u8) (reg u8) (value u8))
        void
        (let via u16 (if (= chip 0)
            page
            (+ page SECOND)))
        (write_at via)))

; the register at i in sent, written only when it changes
(fn reg ((i u8) (value u8)) void
    (if (!= (at sent i) value)
        (send i value)))

(fn word ((p (ptr u8))) u16
    (| (as u16 (load p)) (<< (as u16 (load (+ p 1))) 8)))

; a slot's period, level and mixer into its voice's registers, if it has the voice
(fn out ((s u8)) void
    (if (not (at owns s))
        (return))
    (let t u8 (at tone_at s))
    (let p u16 (at period s))
    (reg t (as u8 p))
    (reg (+ t 1) (as u8 (>> p 8)))
    (let on bool (at playing s))
    (reg (at volume_at s) (if on
        (at level s)
        0))
    (let m u8 (if on
        (at mix s)
        0))
    (let v u8 (at voice s))
    (if (!= m (at voice_mix v))
        (do
            ; the voice's two bits of its chip's mixer, the other voices' as they are
            (set (at voice_mix v) m)
            (let i u8 (at VOICE_MIXER v))
            (let bits u8 (at VOICE_MIX_BITS v))
            (reg i (| (& (at sent i) (~ bits)) (& (at MIX_OFF m) bits))))))

; the effect is over: its voice goes back to the music
(fn end_effect () void
    (set fx_on false)
    (set (at playing FX) false)
    (set (at owns FX) false)
    (set (at owns fx_voice) true)
    (out fx_voice))

; a voice's next pattern from its order list, round to its loop at the end
(fn next_pattern ((v u8)) (ptr u8)
    (let o (ptr u8) (at order v))
    (let off u16 (word o))
    (if (= off 0)
        (do
            (set o (+ song (word (+ o 2))))
            (set off (word o))))
    (set (at order v) (+ o 2))
    (+ song off))

; a note, a rest or a tie
(fn sound ((s u8) (b u8)) void
    (set (at wait s) (at length s))
    (if (= b tune.REST)
        (do
            (set (at level s) 0)
            (if (at owns s)
                (reg (at volume_at s) 0))
            (return)))
    (if (>= b tune.REST)
        (return))
    (set (at period s) (at tune.NOTE_PERIOD b))
    (set (at level s) (at volume s))
    (set (at decay_left s) (at decay s))
    (out s)
    ; the envelope starts again with each note that follows it
    (if (and (= (at volume s) 16) (at owns s))
        (do
            (let shape u8 (+ (at CHIP_BASE (at chip_of s)) 13))
            (send shape (at sent shape)))))

; the bytes up to the next note, rest or tie
(fn events ((s u8)) void
    (let p (ptr u8) (at pos s))
    (let i u8 (at index s))
    (while true
        (let b u8 (load (+ p i)))
        (set i (+ i 1))
        (if (< b tune.LENGTH)
            (do
                (set (at index s) i)
                (sound s b)
                (return)))
        (if (= b tune.END)
            (do
                (if (= s FX)
                    (do
                        (end_effect)
                        (return)))
                (set p (next_pattern s))
                (set (at pos s) p)
                (set i 0)
                (continue)))
        (if (< b tune.VOLUME)
            (do
                (set (at length s) (- b 0x7F))
                (continue)))
        (if (< b tune.DECAY)
            (do
                (set (at volume s) (- b tune.VOLUME))
                (continue)))
        (if (< b tune.MIXER)
            (do
                (set (at decay s) (- b tune.DECAY))
                (continue)))
        (if (< b tune.NOISE)
            (do
                (set (at mix s) (- b tune.MIXER))
                (continue)))
        (let base u8 (at CHIP_BASE (at chip_of s)))
        (if (= b tune.NOISE)
            (do
                (reg (+ base 6) (load (+ p i)))
                (set i (+ i 1))
                (continue)))
        ; the envelope: shape (written by each note that follows), period low, high
        (set (at sent (+ base 13)) (load (+ p i)))
        (reg (+ base 11) (load (+ p (+ i 1))))
        (reg (+ base 12) (load (+ p (+ i 2))))
        (set i (+ i 3))
        (set (at volume s) 16)))

; a note's level goes down 1 every decay ticks
(fn fade ((s u8)) void
    (set (at decay_left s) (- (at decay_left s) 1))
    (if (!= (at decay_left s) 0)
        (return))
    (set (at decay_left s) (at decay s))
    ; silent, or the envelope's: it stays so until the next note
    (let l u8 (at level s))
    (if (or (= l 0) (= l 16))
        (return))
    (let down u8 (- l 1))
    (set (at level s) down)
    (if (at owns s)
        (reg (at volume_at s) down)))

; one timer interrupt: every slot that plays counts its note down, fading it, or reads on
(fn tick () void
    (if (not found)
        (return))
    (if (= (& (io_read u8 (+ page IFR)) 0x40) 0)
        (return))
    (io_read u8 (+ page T1C_L))
    (if any_stale
        (do
            (set any_stale false)
            (for v u8 0 VOICES
                (if (at stale v)
                    (do
                        (set (at stale v) false)
                        (out (if (at owns v)
                            v
                            FX)))))))
    (for s u8 0 SLOTS
        (if (at playing s)
            (do
                (set (at wait s) (- (at wait s) 1))
                (if (= (at wait s) 0)
                    (events s)
                    (if (!= (at decay s) 0)
                        (fade s)))))))
