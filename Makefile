# Copyright 2026 The gsradio Authors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Makefile for gsradio. Run ./configure first; it writes config.mk.
# Only POSIX make features are used, so BSD make and GNU make both work.

.POSIX:
.SUFFIXES:
.SUFFIXES: .c .cc .o

include config.mk

CORE_OBJS = \
	src/config.o \
	src/crossfade.o \
	src/cue.o \
	src/decoder.o \
	src/encoder.o \
	src/log.o \
	src/main.o \
	src/metadata.o \
	src/mp3.o \
	src/playlist.o \
	src/playlist_builtin.o \
	src/shout_stream.o \
	src/signals.o \
	src/source.o \
	src/streamer.o

OBJS = $(CORE_OBJS) $(OPTIONAL_OBJS)

HDRS = \
	src/build_config.h \
	src/config.h \
	src/crossfade.h \
	src/cue.h \
	src/decoder.h \
	src/encoder.h \
	src/gst_util.h \
	src/log.h \
	src/metadata.h \
	src/mp3.h \
	src/perl_glue.h \
	src/playlist.h \
	src/shout_stream.h \
	src/signals.h \
	src/source.h \
	src/streamer.h

all: gsradio

gsradio: $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o gsradio $(OBJS) $(LIBS)

$(OBJS): $(HDRS) config.mk

.cc.o:
	$(CXX) $(CPPFLAGS) -I. $(DEFS) $(PKG_CFLAGS) $(CXXFLAGS) -c -o $@ $<

# Only the Perl glue is C; Perl's headers need Perl's own compiler flags.
.c.o:
	$(CC) $(CPPFLAGS) -I. $(PERL_CFLAGS) $(CFLAGS) -c -o $@ $<

install: gsradio
	mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)/man1
	mkdir -p $(DESTDIR)$(CONF_DIST_DIR) $(DESTDIR)$(MODULE_DIST_DIR)
	$(INSTALL) -m 755 gsradio $(DESTDIR)$(BINDIR)/gsradio
	$(INSTALL) -m 644 doc/gsradio.1 $(DESTDIR)$(MANDIR)/man1/gsradio.1
	$(INSTALL) -m 644 conf/gsradio.conf.dist $(DESTDIR)$(CONF_DIST_DIR)/gsradio.conf.dist
	$(INSTALL) -m 644 conf/gsradio.py.dist $(DESTDIR)$(MODULE_DIST_DIR)/gsradio.py.dist
	$(INSTALL) -m 644 conf/gsradio.pm.dist $(DESTDIR)$(MODULE_DIST_DIR)/gsradio.pm.dist

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/gsradio $(DESTDIR)$(MANDIR)/man1/gsradio.1
	rm -f $(DESTDIR)$(CONF_DIST_DIR)/gsradio.conf.dist
	rm -f $(DESTDIR)$(MODULE_DIST_DIR)/gsradio.py.dist $(DESTDIR)$(MODULE_DIST_DIR)/gsradio.pm.dist

# Integration tests (need gst-launch-1.0, ffmpeg and ffprobe).
check: gsradio
	tests/run_tests.sh ./gsradio

clean:
	rm -f gsradio src/*.o

distclean: clean
	rm -f config.mk config.log src/build_config.h

.PHONY: all install uninstall check clean distclean
