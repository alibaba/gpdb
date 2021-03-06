#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_ctl
#
# Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/bin/pg_ctl/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_ctl - starts/stops/restarts the PostgreSQL server"
PGAPPICON=win32

subdir = src/bin/pg_ctl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# The frontend doesn't need everything that's in LIBS, some are backend only
LIBS := $(filter-out -lresolv -lbz2, $(LIBS))
# This program isn't interactive, so doesn't need these
LIBS := $(filter-out -lreadline -ledit -ltermcap -lncurses -lcurses -lcurl -lssl -lcrypto -lz, $(LIBS))

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += $(libpq_pgport)

ifeq ($(PORTNAME),openbsd)
override CFLAGS += -pthread
endif

OBJS=	pg_ctl.o $(WIN32RES)

all: pg_ctl

pg_ctl: $(OBJS) | submake-libpq submake-libpgport
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_ctl$(X) '$(DESTDIR)$(bindir)/pg_ctl$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_ctl$(X)'

clean distclean maintainer-clean:
	rm -f pg_ctl$(X) $(OBJS)
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
