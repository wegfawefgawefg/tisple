; lo-res stripes; gfx/lores.tsp is imported under the shorter name gr

(import gfx/lores gr)

(fn main
    (gr.show)
    (for y 0 48
        (for x 0 40
            (gr.plot x y (& (+ x y) 15)))))
