#lang racket
(require louDBus/unsafe)
;(define proxy (loudbus-proxy "edu.grinnell.glimmer.guide.SampleServerX"
;                             "/edu/grinnell/glimmer/guide/SampleObjectX"
;                             "edu.grinnell.glimmer.guide.SampleInterfaceX"))


(define service (loudbus-proxy "org.freedesktop.DBus"
                               "/"
                               "org.freedesktop.DBus"))


;(loudbus-methods proxy)
;(map (lambda (fun) (loudbus-method-info proxy fun)) (loudbus-methods proxy))

;(loudbus-method-info proxy 'iadd)

(loudbus-services)

;(dbus-interfaces proxy)
