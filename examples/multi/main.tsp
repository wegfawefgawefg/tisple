; lo-res stripes; gfx/lores.tsp is imported under the shorter name gr

(import gfx/lores gr)

(fn main () void
    (gr.show)
    (for y u8 0 48
        (for x u8 0 40
            (gr.plot x y (& (+ x y) 15)))))
