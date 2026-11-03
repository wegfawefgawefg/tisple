; songs and sound effects for lib/mockingboard, written as text and turned into bytes while
; compiling: (import mockingboard_song tune), then at top level
;   (tune.song music
;       (pattern bass "l6 v13 k2 c2 c2 g1 a#1")
;       (pattern bell "l3 v9 k1 e5 b5 r g5")
;       (voice 0 bass)
;       (voice 3 intro loop bell bell bell2))
;   (tune.effect zap "l1 v15 k1 c7 g6 c6 g5")
; each makes a let, an array of u8: give (addr_of (at music 0)) to mockingboard.play and
; (addr_of (at zap 0)) to mockingboard.effect. Placement options go last and on to the let:
; (tune.song music ... :in aux), (tune.effect zap "..." :align 256). The player reads main
; memory (and the language card), so a song :in aux is copied in with lib/auxmem to be played;
; a song :on disk is a const table, read in with lib/disk (Catwiz's, 2026-11-01).
;
; the text, tokens between spaces. a tick is one timer interrupt (60 a second by default)
;   c4 c#4 ... b8  a note, C1 to B8 (sharps only); sounds for the length
;   r              a rest: silent for the length
;   -              a tie: the note before goes on for the length
;   l6             the length of what follows, 1-64 ticks
;   v12            the volume, 0-15 (and the hardware envelope off)
;   k2             decay: the volume goes down 1 every 2 ticks of a note; k0 holds it
;   m1             the mixer: m1 tone, m2 noise, m3 both, m0 neither
;   n8             the noise period, 0-31 (the chip's: its three voices share it)
;   h14:600        the hardware envelope: shape 14, period 600 (the chip's); notes after it
;                  follow the envelope instead of the volume, and restart it
; pitch that moves (lib/mockingboard_bends): only a program whose handler calls
; mockingboard.service_bends plays these; mockingboard.service plays their bytes wrong
;   w2:10          vibrato: depth 1-4 (about 1/8, 1/4, 1/2, 1 semitone each way), speed the
;   w2:10:12       ticks of one wobble, 2-64; then the ticks of each note before it starts,
;                  0-255 (0 if not given); w0 off. from the next note on
;   p4             glide: each note slides from the pitch before it to its own, a step a tick
;                  of its period >> (8 - speed), speed 1-7 (about 1/8 semitone a tick, doubling
;                  up to about a fifth); p0 off
;   s+4 s-4        slide: each note goes up (s+) or down (s-) from its own pitch for as long
;                  as it sounds, a step a tick as p's, to the end of the periods; s0 off. p and
;                  s are one setting: the last one written holds
;   t+2 t-2        detune: each note's period 1-64 more (lower) or less (higher); t0 none.
;                  a unit is about 3 cents at C3, 12 at A4: two voices on the same line a
;                  unit apart beat against each other, a chorus
; a voice keeps its length, volume, decay, mixer, w, p or s and t from one pattern to the
; next. play starts every voice at l8 v12 k0 m1 w0 p0 t0; an effect at l1 v15 k0 m1 w0 p0 t0.
;
; a song: (pattern name "text") as many as it needs, then (voice n name ...) for each of the
; six voices it uses: 0-2 the first chip's three channels, 3-5 the second's. a voice plays its
; patterns in order, then again from the start, or from after the word loop if there is one:
; (voice 2 intro loop verse verse chorus); or once, with the word end last: (voice 0 sting end)
; stops the voice after its patterns (the tick does nothing more for it). voices with patterns
; of different lengths drift against each other, and come back together after the lengths'
; common multiple.
;
; the bytes (what the player reads):
;   $00-$5F note 0-95 (C1 + n semitones)   $60 rest   $61 tie
;   $80-$BF length (byte - $7F) ticks      $C0-$CF volume   $D0-$DF decay
;   $E0-$E3 mixer   $E4 n noise period   $E5 s lo hi envelope shape s, period lo + 256 hi
;   $E6 k r d vibrato: the shift k (8 - depth, 0 off), phase step r (256 / speed), delay d
;   $E7 b bend: 0 none, $10 + k glide, $20 + k slide up, $30 + k down (k = 8 - speed)
;   $E8 lo hi detune, a signed u16 added to each note's period
;   $FF the end of the pattern
; a song is a header of six u16 offsets (from the song's first byte; 0 for a silent voice),
; each to its voice's order list: u16 pattern offsets, then $0000 and the offset of the
; entry to loop back to (0: the voice ends). then the patterns. an effect is one pattern.

(import tables)

(const NOTES u8 96)
(const REST u8 0x60)
(const TIE u8 0x61)
(const LENGTH u8 0x80)
(const VOLUME u8 0xC0)
(const DECAY u8 0xD0)
(const MIXER u8 0xE0)
(const NOISE u8 0xE4)
(const ENVELOPE u8 0xE5)
(const VIBRATO u8 0xE6) ; then its depth's shift (0 off), phase step and delay
(const BEND u8 0xE7)    ; then $10 + k glide, $20 + k slide up, $30 + k slide down (0 none)
(const DETUNE u8 0xE8)  ; then a u16 added to each note's period, low byte first (signed)
(const END u8 0xFF)

; 2 to the 1/12, by Newton's method on r^12 = 2
(macro semitone_ratio ()
    (let r (/ 106 (float 100)))
    (for i 0 20
        (let p11 (float 1))
        (for k 0 11
            (set p11 (* p11 r)))
        (set r (- r (/ (- (* p11 r) 2) (* 12 p11)))))
    r)

; the AY's tone period for note n at the card's clock (the Apple's 1020484 Hz): the clock /
; (16 f), with A4 (n = 45) at 440 Hz
(macro note_period (n)
    (let ratio (semitone_ratio))
    (let f (float 440))
    (let k (- n 45))
    (while (> k 0)
        (set f (* f ratio))
        (set k (- k 1)))
    (while (< k 0)
        (set f (/ f ratio))
        (set k (+ k 1)))
    (round (/ (float 1020484) (* 16 f))))

(tables.table NOTE_PERIOD u16 96 note_period)

; the number in codes from i up to a space or the stop character: (value next)
(macro number_at (codes i stop where label)
    (let value 0)
    (let digits 0)
    (while (and (< i (len codes)) (and (!= (nth codes i) ' ') (!= (nth codes i) stop)))
        (let c (nth codes i))
        (if (or (< c '0') (> c '9'))
            (error "pattern " where ": " label " wants a number"))
        (set value (+ (* value 10) (- c '0')))
        (set digits (+ digits 1))
        (set i (+ i 1)))
    (if (= digits 0)
        (error "pattern " where ": " label " wants a number"))
    (list value i))

(macro in_range (value low high where label)
    (if (or (< value low) (> value high))
        (error "pattern " where ": " label " is " value ", not " low "-" high))
    value)

; c d e f g a b from the letter's code, -1 for any other
(macro semitone (c)
    (match c
        ('c' 0)
        ('d' 2)
        ('e' 4)
        ('f' 5)
        ('g' 7)
        ('a' 9)
        ('b' 11)
        (_ -1)))

; a command letter and its number: (bytes 0 next)
(macro command (codes i base low high where label)
    (let got (number_at codes (+ i 1) 0 where label))
    (list (list (+ base (in_range (nth got 0) low high where label))) 0 (nth got 1)))

; h shape:period
(macro envelope_bytes (codes i where)
    (let shape (number_at codes (+ i 1) ':' where "h"))
    (let colon (nth shape 1))
    (if (or (>= colon (len codes)) (!= (nth codes colon) ':'))
        (error "pattern " where ": h wants shape:period, as h14:600"))
    (let period (number_at codes (+ colon 1) 0 where "h's period"))
    (let p (in_range (nth period 0) 0 65535 where "h's period"))
    (list (list ENVELOPE (in_range (nth shape 0) 0 15 where "h's shape") (% p 256) (/ p 256))
        0
        (nth period 1)))

; w depth:speed or w depth:speed:delay; w0 off. The depth 1-4 is the shift 7-4 of the note's
; period (about 1/8, 1/4, 1/2, 1 semitone each way), the speed the ticks of one wobble (its
; phase steps 256 / speed a tick), the delay the ticks of each note before it starts
(macro vibrato_bytes (codes i where)
    (let depth (number_at codes (+ i 1) ':' where "w"))
    (let d (in_range (nth depth 0) 0 4 where "w's depth"))
    (let after (nth depth 1))
    (let colon (and (< after (len codes)) (= (nth codes after) ':')))
    (if (= d 0)
        (do
            (if colon
                (error "pattern " where ": w0 turns the vibrato off and takes nothing after it"))
            (return (list (list VIBRATO 0 0 0) 0 after))))
    (if (not colon)
        (error "pattern " where ": w wants depth:speed, as w2:10, or depth:speed:delay"))
    (let speed (number_at codes (+ after 1) ':' where "w's speed"))
    (let ticks (in_range (nth speed 0) 2 64 where "w's speed"))
    (let next (nth speed 1))
    (let delay 0)
    (if (and (< next (len codes)) (= (nth codes next) ':'))
        (do
            (let got (number_at codes (+ next 1) 0 where "w's delay"))
            (set delay (in_range (nth got 0) 0 255 where "w's delay"))
            (set next (nth got 1))))
    (list (list VIBRATO (- 8 d) (/ (+ 256 (/ ticks 2)) ticks) delay) 0 next))

; p speed (glide) or s+speed / s-speed (slide): the step a tick is the note's period shifted
; right by 8 - speed, speed 1-7 (about 1/8 semitone a tick, doubling to about a fifth); p0 and
; s0 turn either off
(macro bend_bytes (codes i where label mode)
    (let from (+ i 1))
    (if (= label "s")
        (do
            (let sign (if (< from (len codes))
                (nth codes from)
                0))
            (if (= sign '+')
                (set mode 0x20))
            (if (= sign '-')
                (set mode 0x30))
            (if (!= mode 0)
                (set from (+ from 1)))))
    (let got (number_at codes from 0 where label))
    (let speed (in_range (nth got 0) 0 7 where label))
    (if (and (= speed 0) (> mode 0x10))
        (error "pattern " where ": s0 turns the slide off; s+n and s-n slide"))
    (if (and (!= speed 0) (= mode 0))
        (error "pattern " where ": s wants a direction, as s+3 (up) or s-3 (down)"))
    (list
        (list BEND (if (= speed 0)
            0
            (+ mode (- 8 speed))))
        0
        (nth got 1)))

; t+n or t-n: each note's period n more (lower) or n less (higher), 1-64; t0 none. A unit is
; about 3 cents at C3, 12 at A4 and 24 at A5: two voices a unit apart beat a few times a second
(macro detune_bytes (codes i where)
    (let from (+ i 1))
    (let sign (if (< from (len codes))
        (nth codes from)
        0))
    (let minus (= sign '-'))
    (if (or minus (= sign '+'))
        (set from (+ from 1)))
    (let got (number_at codes from 0 where "t"))
    (let n (in_range (nth got 0) 0 64 where "t"))
    (if (and (!= n 0) (and (not minus) (!= sign '+')))
        (error "pattern " where ": t wants a sign, as t+2 (lower) or t-2 (higher)"))
    (let value (if minus
        (- 65536 n)
        n))
    (list (list DETUNE (% value 256) (/ value 256)) 0 (nth got 1)))

; one token's bytes: (bytes sounds next), sounds 1 for a note, rest or tie
(macro token_bytes (codes i where)
    (let c (nth codes i))
    (let semi (semitone c))
    (if (>= semi 0)
        (do
            (let sharp (if (and (< (+ i 1) (len codes)) (= (nth codes (+ i 1)) '#'))
                1
                0))
            (let got (number_at codes (+ (+ i 1) sharp) 0 where "a note's octave"))
            (let octave (in_range (nth got 0) 1 8 where "an octave"))
            (let n (+ (+ (* (- octave 1) 12) semi) sharp))
            (return (list (list (in_range n 0 95 where "a note")) 1 (nth got 1)))))
    (match c
        ('r' (list (list REST) 1 (+ i 1)))
        ('-' (list (list TIE) 1 (+ i 1)))
        ('l'
            (do
                (let got (number_at codes (+ i 1) 0 where "l"))
                (list (list (+ LENGTH (- (in_range (nth got 0) 1 64 where "l") 1))) 0 (nth got 1))))
        ('v' (command codes i VOLUME 0 15 where "v"))
        ('k' (command codes i DECAY 0 15 where "k"))
        ('m' (command codes i MIXER 0 3 where "m"))
        ('n'
            (do
                (let got (number_at codes (+ i 1) 0 where "n"))
                (list (list NOISE (in_range (nth got 0) 0 31 where "n")) 0 (nth got 1))))
        ('h' (envelope_bytes codes i where))
        ('w' (vibrato_bytes codes i where))
        ('p' (bend_bytes codes i where "p" 0x10))
        ('s' (bend_bytes codes i where "s" 0))
        ('t' (detune_bytes codes i where))
        (_
            (error "pattern " where ": a token starts with the character code " c
                "; write notes as c4 c#4 ..., then r - l v k m n h w p s t"))))

; a pattern's text as bytes, the END after them
(macro pattern_bytes (name text)
    (let codes (chars text))
    (let bytes (list))
    (let sounds 0)
    (let i 0)
    (while (< i (len codes))
        (if (= (nth codes i) ' ')
            (do
                (set i (+ i 1))
                (continue)))
        (let got (token_bytes codes i name))
        (set bytes (append bytes (nth got 0)))
        (set sounds (+ sounds (nth got 1)))
        (set i (nth got 2)))
    (if (= sounds 0)
        (error "pattern " name " has no note, rest or tie; the player would never leave it"))
    (let size (+ (len bytes) 1))
    (if (> size 255)
        (error "pattern " name " is " size " bytes, the player reads 255; split it in two"))
    (append bytes (list END)))

(macro low_high (value)
    (list (% value 256) (/ value 256)))

; the place of name in names, or an error
(macro index_of (names name voice)
    (for k 0 (len names)
        (if (= (nth names k) name)
            (return k)))
    (error "voice " voice " names no pattern " name))

(macro song (name & parts)
    (let options (list))
    (let names (list))
    (let patterns (list))
    (let voices (repeat 6 (list)))
    (let loops (repeat 6 0))
    (let used (repeat 6 false))
    (for p 0 (len parts)
        (let part (nth parts p))
        ; the let's placement options, from the first keyword on
        (if (or (!= (len options) 0) (= (kind part) 'keyword))
            (do
                (set options (append options (list part)))
                (continue)))
        (match (head part)
            ('pattern
                (set names (append names (list (nth part 1))))
                (set patterns (append patterns (list (pattern_bytes (nth part 1) (nth part 2))))))
            ('voice
                (let v (nth part 1))
                (if (or (!= (kind v) 'int) (or (< v 0) (> v 5)))
                    (error "a voice is 0-5, not " v))
                (let order (list))
                (let loop_at 0)
                (for k 2 (len part)
                    (if (= (nth part k) 'loop)
                        (set loop_at (len order))
                        (if (= (nth part k) 'end)
                            (do
                                (if (!= k (- (len part) 1))
                                    (error "voice " v ": end goes last"))
                                (set loop_at -1))
                            (set order (append order (list (nth part k)))))))
                (if (>= loop_at (len order))
                    (error "voice " v " has nothing to play after loop"))
                (let new_voices (list))
                (let new_loops (list))
                (let new_used (list))
                (for k 0 6
                    (set new_voices
                        (append new_voices
                            (list (if (= k v)
                                order
                                (nth voices k)))))
                    (set new_loops
                        (append new_loops
                            (list (if (= k v)
                                loop_at
                                (nth loops k)))))
                    (set new_used (append new_used (list (or (= k v) (nth used k))))))
                (set voices new_voices)
                (set loops new_loops)
                (set used new_used))
            (_
                (error "a song has (pattern name \"text\") and (voice n name ...), then placement "
                    "options, not " part))))
    ; the order lists after the header, then the patterns
    (let offset 12)
    (let order_at (list))
    (for v 0 6
        (set order_at (append order_at (list offset)))
        (if (nth used v)
            (set offset (+ offset (+ (* 2 (len (nth voices v))) 4)))))
    (let pattern_at (list))
    (for k 0 (len patterns)
        (set pattern_at (append pattern_at (list offset)))
        (set offset (+ offset (len (nth patterns k)))))
    (let bytes (list))
    (for v 0 6
        (set bytes
            (append bytes
                (low_high (if (nth used v)
                    (nth order_at v)
                    0)))))
    (for v 0 6
        (if (nth used v)
            (do
                (let order (nth voices v))
                (for k 0 (len order)
                    (let which (index_of names (nth order k) v))
                    (set bytes (append bytes (low_high (nth pattern_at which)))))
                (set bytes (append bytes (list 0 0)))
                (set bytes
                    (append bytes
                        (low_high (if (< (nth loops v) 0)
                            0
                            (+ (nth order_at v) (* 2 (nth loops v))))))))))
    (for k 0 (len patterns)
        (set bytes (append bytes (nth patterns k))))
    ; :on disk takes a const (a program reads the song into memory with lib/disk, then plays it)
    (let on_disk false)
    (for k 0 (len options)
        (if (= (nth options k) :on)
            (set on_disk true)))
    (if on_disk
        `(const ,name (array u8 ,(len bytes)) (make (array u8 ,(len bytes)) ,@bytes) ,@options)
        `(let ,name (array u8 ,(len bytes)) (make (array u8 ,(len bytes)) ,@bytes) ,@options)))

(macro effect (name text & options)
    (let bytes (pattern_bytes name text))
    `(let ,name (array u8 ,(len bytes)) (make (array u8 ,(len bytes)) ,@bytes) ,@options))
