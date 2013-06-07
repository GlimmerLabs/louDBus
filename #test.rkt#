#lang racket

;;; test.rkt
;;;   A sample D-Bus client.
;;;
;;; Copyright (c) 2012 Samuel A. Rebelsky.  All rights reserved.

(provide server
         server-reset!
         server-proxy)

(require loudbus/unsafe)
; (require "unsafe.rkt")

; Make a proxy to the server
(define server-proxy
  (lambda ()
    (loudbus-proxy "edu.grinnell.cs.glimmer.testserver"
                   "/edu/grinnell/cs/glimmer/TestServer"
                   "edu.grinnell.cs.glimmer.TestServer")))

; Reset the client.  (In some cases, the client can get lost, so
; this gives us a way to reactivate it without quitting.
(define server-reset!
  (lambda ()
    (set! server (server-proxy))
    ; (loudbus-import server "test." #f)
    ; (loudbus-import server "" #t)
    ))

; The actual server client.  It can be used for things like adbc-call
; and adbc-import (see below).
(define server '())

; And set things to work
(server-reset!)
