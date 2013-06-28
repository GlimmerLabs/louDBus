#lang racket

(require louDBus/unsafe)

(define gimp (loudbus-proxy "edu.grinnell.cs.glimmer.GimpDBus"
                            "/edu/grinnell/cs/glimmer/gimp"
                            "edu.grinnell.cs.glimmer.pdb"))
(define expt-bytes-put
  (lambda ()
    (let ((data (bytes 1 255 0 126 0 22 31 8 1)))
      (loudbus-call gimp 'test_bytes_put (bytes-length data) data))))

(expt-bytes-put)
