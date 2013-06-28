#lang racket

;;; File:
;;;   louDBus/gimp-experiments.rkt
;;; Author:
;;;   Samuel A. Rebelsky
;;; Summary:
;;;   A collection of experiments to check if louDBus is working okay.
;;;   These experiments require that the gimp-dbus server is available.

(require louDBus/unsafe)

(define gimp (loudbus-proxy "edu.grinnell.cs.glimmer.GimpDBus"
                            "/edu/grinnell/cs/glimmer/gimp"
                            "edu.grinnell.cs.glimmer.pdb"))

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Experiment 0x - Legal functions calls.

; Experiment 01 - Legal function call to gimp-image-new.
(define expt01
  (lambda ()
    (loudbus-call gimp 'gimp_image_new 200 200 0)))

; Experiment 02 - Legal function call using dashes
(define expt02
  (lambda ()
    (loudbus-call gimp 'gimp-image-new 200 200 0)))

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Experiment 1x - Illegal function calls

; Experiment 11 - Invalid function name
(define expt11
  (lambda ()
    (loudbus-call gimp 'fdafdssadf)))

; Experiment 12 - Invalid number of parameters
(define expt12
  (lambda ()
    (loudbus-call gimp 'gimp-image-new 200)))

; Experiment 13 - Invalid parameter types
(define expt13
  (lambda ()
    (loudbus-call gimp 'gimp-image-new 200 200 1.5)))

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Experiment 2x - loudbus-method-info

; Experiment 21 - A valid request
(define expt21
  (lambda ()
    (loudbus-method-info gimp 'gimp_image_new)))

; Experiment 22 - An invalid request
(define expt22
  (lambda ()
    (loudbus-method-info gimp 'adsfasdfa)))

; Experiment 23 - Using dashes in the function name
(define expt23
  (lambda ()
    (loudbus-method-info gimp 'gimp-image-new)))

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Experiment 3x - loudbus-services

; Experiment 31 - The basic test.  (I'm not sure that there are others.)
(define expt31
  (lambda ()
    (loudbus-services)))

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Experiment 4x - loudbus-objects

; Experiment 41 - All objects for gimp
(define expt41
  (lambda ()
    (loudbus-objects gimp)))

; Experiment 42 - Illegal service
(define expt42
  (lambda ()
    (loudbus-objects "abc")))

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Experiment 9x - 

; Experiment 91  - How well do we handle arrays of bytes?
(define expt91
  (lambda ()
    (loudbus-call gimp 'test_bytes_get)))
