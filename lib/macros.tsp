; small macros: (import macros) then (macros.inc x), (macros.dec x),
; (macros.unroll i 0 8 body ...)

(macro inc (place)
    `(set ,place (+ ,place 1)))

(macro dec (place)
    `(set ,place (- ,place 1)))

; the body once for each i from start up to end, with i a literal in each copy: no loop
; counter, no compare, no branch, and (at table i) becomes a fixed address. Each copy is its
; own do, so its lets don't clash
(macro unroll (name start end & body)
    (let copies (list))
    (for i start end
        (let copy `(do
            ,@(replace body name i)))
        (set copies (append copies (list copy))))
    `(do
        ,@copies))

; form with every old in it, at any depth, swapped for new
(macro replace (form old new)
    (if (= form old)
        (return new))
    (if (!= (kind form) 'list)
        (return form))
    (let out (list))
    (for i 0 (len form)
        (set out (append out (list (replace (nth form i) old new)))))
    out)
