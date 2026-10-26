; macros are templates: ` builds a form, , drops an argument in, ,@ splices a list of forms

(macro swap (a b)
    `(do
        (let tmp# u8 ,a) ; tmp# is a fresh name every expansion
        (set ,a ,b)
        (set ,b tmp#)))

(macro inc (place)
    `(set ,place (+ ,place 1)))

; body is a list of forms, spliced into the loop
(macro times (n body)
    `(for i# u8 0 ,n
        ,@body))

(fn shuffle ((a u8) (b u8)) u8
    (let x u8 a)
    (let y u8 b)
    (swap x y)
    (times 3 ((inc x) (inc y)))
    (+ x y))
