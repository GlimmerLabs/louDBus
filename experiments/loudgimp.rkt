#lang racket

; A quick hack to see if I can connct to the GIMP.

(require louDBus/unsafe)

(define gimp (loudbus-proxy "edu.grinnell.cs.glimmer.GimpDBus"
                            "/edu/grinnell/cs/glimmer/gimp"
                            "edu.grinnell.cs.glimmer.pdb"))


(loudbus-call gimp 'gimp-context-get-font)
