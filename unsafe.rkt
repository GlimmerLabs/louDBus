#lang racket

;;; adbc-psr/unsafe.rkt
;;;   The primary module for A D-Bus Client for PLT Scheme and Racket.
;;;
;;; Copyright (c) 2012 Samuel A. Rebelsky
;;; INSERT GNU LICENSE

(provide loudbus-call
         loudbus-import
         loudbus-methods
         loudbus-proxy)

; We'll be using the FFI, mostly to ensure that we have the various
; GLib libraries loaded.  The FFI is unsafe, and so we note that this
; is equally unsafe.
(require ffi/unsafe
         ffi/unsafe/define)

; We will be using various parts of GLib.  This is one way to load
; those parts into the runtime.
(define glib-2.0 (ffi-lib "libglib-2.0"))
(define gobject-2.0 (ffi-lib "libgobject-2.0"))
(define gio-2.0 (ffi-lib "libgio-2.0"))

; Set up a pointer type.
(define _LouDBusProxy* (_cpointer 'LouDBusProxy))

; Set up the library, which is built using the Inside Racket
; API and should therefore be treated as a module.
(require "loudbus")

; Initialize louDBus and tell it about the pointer type.
(loudbus-init _LouDBusProxy*)
