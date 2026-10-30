; the interrupt side of lib/mockingboard: (mockingboard.service) in the program's :interrupt
; handler runs tick every timer interrupt. The state is plain globals: mockingboard.tsp's calls
; from main set them with interrupts off, and nothing here is called from main, so the
; handler has its own copy of every function it runs.
;
; Each tick: answer the 6522, then step the six music voices and the effect. A register is
; written when what a voice wants in it differs from what was written last; a write is six
; 6522 accesses at the slot's own addresses, a match on the slot picking the copy.

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
(let port u8 0)         ; (slot - 1) * 2: send's arm for the first chip, + 1 the second
(let orb (array u16 2)) ; each chip's 6522 port B: its bus lines (for main's writes)
(let ora (array u16 2)) ; port A: its data bus
(let ifr u16 0)         ; the first 6522's flags
(let t1_low u16 0)      ; reading it answers the timer

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

; a place in sent as a chip and its register
(const SENT_CHIP (array u8 28)
    (make (array u8 28) 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1))

(const SENT_REG (array u8 28)
    (make (array u8 28) 0 1 2 3 4 5 6 7 8 9 10 11 12 13 0 1 2 3 4 5 6 7 8 9 10 11 12 13))

; one register write at page (a 6522's): the number latched, then the value written, each
; through the inactive state between
(macro write_at (page)
    `(do
        (io_write u8 ,(+ page 1) reg)
        (io_write u8 ,page BUS_LATCH)
        (io_write u8 ,page BUS_INACTIVE)
        (io_write u8 ,(+ page 1) value)
        (io_write u8 ,page BUS_WRITE)
        (io_write u8 ,page BUS_INACTIVE)))

; the handler's register write, the register at i in sent (0-13 the first chip's, 14-27 the
; second's): an arm for each slot and chip, so every access is to a fixed address (6 cycles;
; a computed one takes 20)
(macro fixed_writer (name)
    (let arms (list))
    (for n 1 8
        (for chip 0 2
            (let page (+ 0xC000 (+ (* n 256) (* chip 128))))
            (set arms (append arms (list `(,(+ (* (- n 1) 2) chip) (write_at ,page)))))))
    `(fn ,name
        ((i u8) (value u8))
        void
        (set (at sent i) value)
        (let reg u8 (at SENT_REG i))
        (match (+ port (at SENT_CHIP i))
            ,@arms
            (_))))

(fixed_writer send)

; main's: through the addresses init found, for the few writes it makes
(macro psg_writer (name)
    `(fn ,name
        ((chip u8) (reg u8) (value u8))
        void
        (let b u16 (at orb chip))
        (let a u16 (at ora chip))
        (io_write u8 a reg)
        (io_write u8 b BUS_LATCH)
        (io_write u8 b BUS_INACTIVE)
        (io_write u8 a value)
        (io_write u8 b BUS_WRITE)
        (io_write u8 b BUS_INACTIVE)))

; the register at i in sent, written only when it changes
(fn reg ((i u8) (value u8)) void
    (if (!= (at sent i) value)
        (send i value)))

(fn word ((p (ptr u8))) u16
    (| (as u16 (load p)) (<< (as u16 (load (+ p 1))) 8)))

; a chip's mixer register from what its three voices want (1 turns a tone or noise off)
(fn mixer ((chip u8)) void
    (let r u8 0x3F)
    (let first u8 (* chip 3))
    (for c u8 0 3
        (let m u8 (at voice_mix (+ first c)))
        (if (!= (& m 1) 0)
            (set r (& r (~ (<< 1 c)))))
        (if (!= (& m 2) 0)
            (set r (& r (~ (<< 8 c))))))
    (reg (+ (at CHIP_BASE chip) 7) r))

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
            (set (at voice_mix v) m)
            (mixer (at chip_of s)))))

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
    (let l u8 (at level s))
    (if (or (= l 0) (= l 16))
        (return))
    (let left u8 (- (at decay_left s) 1))
    (if (> left 0)
        (do
            (set (at decay_left s) left)
            (return)))
    (set (at decay_left s) (at decay s))
    (set (at level s) (- l 1))
    (if (at owns s)
        (reg (at volume_at s) (- l 1))))

; one timer interrupt: every slot that plays counts its note down, fading it, or reads on
(fn tick () void
    (if (not found)
        (return))
    (if (= (& (io_read u8 ifr) 0x40) 0)
        (return))
    (io_read u8 t1_low)
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
                (let w u8 (at wait s))
                (if (> w 1)
                    (do
                        (set (at wait s) (- w 1))
                        (if (!= (at decay s) 0)
                            (fade s)))
                    (events s))))))
