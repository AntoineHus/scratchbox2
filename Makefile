CC = gcc
LD = ld
PACKAGE_VERSION = "1.99b"
PACKAGE = "SB2"
CFLAGS = -Wall -W -I$(TOPDIR)/include -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE

TOPDIR = $(CURDIR)

MAKEFILES = Makefile $(TOPDIR)/Makefile.include

export CC CFLAGS MAKEFILES TOPDIR

subdirs = lua preload utils
targets = utils/sb2init


all: build

configure: configure.ac
	./autogen.sh
	./configure

build: configure
	$(ll_toplevel_build)
	@echo Build completed successfully!

install: build
	install -d -m 755 $(prefix)/scratchbox/bin
	install -d -m 755 $(prefix)/scratchbox/lib
	install -d -m 755 $(prefix)/scratchbox/redir_scripts
	install -c -m 755 preload/libsb2.so $(prefix)/scratchbox/lib/libsb2.so
	install -c -m 755 utils/sb2init $(prefix)/scratchbox/bin/sb2init
	install -c -m 755 scripts/login.sh $(prefix)/login.sh
	install -c -m 755 scripts/sb2rc $(prefix)/scratchbox/sb2rc
	install -c -m 644 redir_scripts/main.lua $(prefix)/scratchbox/redir_scripts/main.lua


CLEAN_FILES = $(targets)

clean:
	$(ll_clean)

-include .config
include Makefile.include


