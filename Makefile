# louDBus/Makefile
#   A Makefile for A D-Bus Client for PLT Scheme and Racket

# +-----------------------+-------------------------------------------
# | Configurable Settings |
# +-----------------------+

# These may need to be changed on various systems.

# The current version of the system
VERSION = 0.1

INSTALL_DIR = /glimmer/lib/louDBus

# +-------+-----------------------------------------------------------
# | Files |
# +-------+

# All of the sources we use.  Important primarily for packaging.

RACKET_SOURCES = \
        unsafe.rkt \
        test.rkt \
        compiled-goes-here.rkt

C_SOURCES = \
        loudbus.c 

SCRIPTS = \
        racocflags \
        racocppflags 

OTHER_FILES = \
        Makefile \
        README \
        MANIFEST \
        INSTALL \
        DETAILS \
        TODO

FILES = \
        $(C_SOURCES) \
        $(RACKET_SOURCES) \
        $(SCRIPTS) \
        $(OTHER_FILES)

# +----------+--------------------------------------------------------
# | Settings |
# +----------+

CFLAGS = -g -Wall -fPIC \
	$(shell pkg-config --cflags gio-2.0 glib-2.0 gio-unix-2.0)  \
	-I/usr/include/racket 

RACO_CFLAGS = $(shell echo '' $(CFLAGS) | ./racocflags)

LDLIBS = $(shell pkg-config --libs gio-2.0 glib-2.0 gio-unix-2.0)

RACO_LDLIBS = ++ldl -lgio-2.0 ++ldl -lgobject-2.0 ++ldl -lglib-2.0 

# Inside Racket says to use --cgc, but that requires mzdyn.o, which does
# not seem to ship with the standard distribution, and I'm lazy.
RACO_GC = --3m

# We need to know where to put the compiled Racket library.  
COMPILED_DIR = $(shell racket compiled-goes-here.rkt)

# +------------------+------------------------------------------------
# | Standard Targets |
# +------------------+

default: build

build: $(COMPILED_DIR)/loudbus.so 

clean:
	rm -f *.o
	rm -f *.so
	rm -rf compiled
	rm -rf louDBus-$(VERSION)
	rm -rf *.tar.gz
	rm -rf *~

package: louDBus-$(VERSION).tar.gz

install-local: build
	raco link `pwd`

install: build compile
	mkdir -p $(INSTALL_DIR)
	cp -r compiled $(INSTALL_DIR)
	cp unsafe.rkt $(INSTALL_DIR)

.PHONY: compile
compile:
	raco make unsafe.rkt

# +---------+---------------------------------------------------------
# | Details |
# +---------+

# Making tar files.

louDBus-$(VERSION).tar.gz: $(FILES)
	rm -rf louDBus.$(VERSION)
	mkdir louDBus.$(VERSION)
	cp $(FILES) louDBus-$(VERSION)
	tar cvfa $@ louDBus-$(VERSION)

# Making the louDBus library (using the Inside Racket API)

loudbus.o: loudbus.c
	raco ctool --cc $(RACO_GC) $(RACO_CFLAGS) $^

loudbus.so: loudbus.o
	raco ctool --vv $(RACO_GC) ++ldf -L/usr/lib/x86_64-linux-gnu $(RACO_LDLIBS) --ld $@ $^

# The louDBus library needs to go into the directory for compiled modules.
$(COMPILED_DIR)/loudbus.so: loudbus.so
	install -D $^ $@

# +---------------------+---------------------------------------------
# | Header Dependencies |
# +---------------------+

libadbc.o: adbc.h
adbc-psr.o: 

# +-------------+-----------------------------------------------------
# | Experiments |
# +-------------+

.PHONY: preprocess
preprocess:
	$(CC) $(CFLAGS) -E adbc-psr.c | less

.PHONY: rflags
rflags:
	echo 'RACO_FLAGS' $(RACO_CFLAGS)
