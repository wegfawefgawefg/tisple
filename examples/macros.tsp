; macros are templates: ` builds a form, , drops an argument in, ,@ splices a list of forms

(macro swap (args a b)
    `(do
        (let tmp# ,a) ; tmp# is a fresh name every expansion
        (set ,a ,b)
        (set ,b tmp#)))

(macro inc (args place)
    `(set ,place (+ ,place 1)))

; body is a list of forms, spliced into the loop
(macro times (args n body)
    `(for i# 0 ,n
        ,@body))

(fn shuffle (args (a u8) (b u8))
    (let x a)
    (let y b)
    (swap x y)
    (times 3 ((inc x) (inc y)))
    (+ x y))
