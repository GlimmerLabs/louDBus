#lang racket

; A simple program that tells us where to put compiled code.

(display (build-path "compiled" "native" (system-library-subpath)))
(newline)
