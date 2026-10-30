; music and effects on a Mockingboard (slot 4 usually; any slot is found): loops of
; different lengths phasing over each other, and effects on keys 1-4. ESC ends it. With no
; card the effects play on the speaker and the music is silent.
;
; the loops, in pulses of 8 ticks (0.13 s), all in D dorian: a bass of 16, an arpeggio of 12
; and the same arpeggio with a rest, 13, in the other speaker, so the two slide apart one
; pulse a bar and come back together after 156; a bell of 5, a pad of 64 and a shaker of 3

(import apple2 a2)
(import mockingboard mb)
(import mockingboard_song tune)
(import text40)

(tune.song music
    (pattern bass "l8 v11 k3 d2 - r d2 - r a1 - c2 - r d2 - r f2 -")
    (pattern arp "l8 v11 k1 d4 a4 c5 e5 a4 d5 c5 a4 g4 a4 e5 c5")
    (pattern pad "l64 v8 k15 f3 - a3 - g3 - e3 -")
    (pattern arp_slip "l8 v10 k1 d4 a4 c5 e5 a4 d5 c5 a4 g4 a4 e5 c5 r")
    (pattern bell_in "l32 r")
    (pattern bell "l8 v7 k1 a5 r e6 r d6")
    (pattern shaker "l8 m2 n2 k2 v6 c4 v3 c4 r")
    (voice 0 bass)
    (voice 1 arp)
    (voice 2 pad)
    (voice 3 arp_slip)
    (voice 4 bell_in loop bell)
    (voice 5 shaker))

(tune.effect zap "l1 v15 c7 g6 d6 a5 e5 b4 f#4 c#4 g#3")
(tune.effect coin "l4 v14 b5 l12 k1 e6")
(tune.effect boom "m2 n31 l2 v15 c4 l30 k2 -")
(tune.effect chime "h0:1200 l40 c6")

(fn irq () void :interrupt
    (mb.service))

(fn title () void
    (text40.move 2 2)
    (text40.print "MOCKINGBOARD")
    (text40.move 2 4)
    (if mb.found
        (do
            (text40.print "SLOT ")
            (text40.put (+ '0' mb.slot)))
        (text40.print "NO CARD: EFFECTS ON THE SPEAKER"))
    (text40.move 2 7)
    (text40.print "1 ZAP  2 COIN  3 BOOM  4 CHIME")
    (text40.move 2 9)
    (text40.print "ESC TO STOP"))

; the waiting key, or 0 (lib/keys has the same, but examples/keys.tsp takes its name here)
(fn read_key () u8
    (let k u8 (io_read u8 a2.KBD))
    (if (< k 0x80)
        (return 0))
    (io_read u8 a2.KBDSTRB)
    (& k 0x7F))

(fn main () void
    (text40.on)
    (mb.init)
    (title)
    (mb.play (addr_of (at music 0)))
    (while true
        (let k u8 (read_key))
        (match k
            (0x31 (mb.effect (addr_of (at zap 0)) 5 1))
            (0x32 (mb.effect (addr_of (at coin 0)) 5 2))
            (0x33 (mb.effect (addr_of (at boom 0)) 5 3))
            (0x34 (mb.effect (addr_of (at chime 0)) 5 1))
            (a2.KEY_ESC
                (mb.off)
                (return))
            (_))))
