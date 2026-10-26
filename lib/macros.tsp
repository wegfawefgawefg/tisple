; small macros: (import macros) then (macros.inc x), (macros.dec x)

(macro inc (place)
    `(set ,place (+ ,place 1)))

(macro dec (place)
    `(set ,place (- ,place 1)))
