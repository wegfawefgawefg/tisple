; opt/eval.cpp remembered (zdiv 0), worked out in an early round for f0 (which never runs), as
; 0 for the rest of the compile. Later vars made x a literal in zdiv's body (every running
; call passes 65535), args passed 0 for the unread x, and f2's call became (zdiv 0): the
; remembered 0, where the body now gives 5046. Values are remembered for one round now
(let zh u16 29695)
(let g6 u16)
(let g8 bool)
(let arr11 (array u16 4) (make (array u16 4) 65535 65535 65535 11860))
(fn zdiv ((x u16)) u16 (+ (/ x 13) (% x 10)))
(fn f0 () void (set zh (+ zh (zdiv g6))))
(fn f2 ((a u16)) void (set zh (+ zh (zdiv (if g8 (at arr11 1) a)))))
(fn main () u8
    (f2 (at arr11 2))
    (as u8 zh))
