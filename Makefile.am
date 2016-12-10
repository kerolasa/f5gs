AUTOMAKE_OPTIONS = gnu
ACLOCAL_AMFLAGS = -I m4
AM_CPPFLAGS = \
	-include config.h \
	-I$(top_srcdir)/lib \
	-I$(top_builddir)/lib \
	-DF5GS_POST=\"$(f5gs_post)\" \
	-DF5GS_PRE=\"$(f5gs_pre)\" \
	-DF5GS_RUNDIR=\"$(f5gs_rundir)\"

EXTRA_DIST = \
	m4/gnulib-cache.m4 \
	f5gs.8.in \
	contrib/f5gs.service.in \
	contrib/f5gs.socket.in \
	contrib/init.redhat

SUBDIRS = lib

bin_PROGRAMS = f5gs
f5gs_SOURCES = f5gs.c client.c server.c f5gs.h
f5gs_LDADD = \
	$(top_builddir)/lib/lib$(PACKAGE).la \
	-lrt \
	$(GETADDRINFO_LIB) \
	$(HOSTENT_LIB) \
	$(INET_NTOP_LIB) \
	$(LIBSOCKET) \
	$(LIB_POLL) \
	$(LIB_SELECT) \
	$(LTLIBINTL) \
	$(LTLIBTHREAD) \
	$(SERVENT_LIB)

if HAVE_SYSTEMD
f5gs_CFLAGS = \
	$(SYSTEMD_DAEMON_CFLAGS) \
	$(SYSTEMD_JOURNAL_CFLAGS) \
	$(SYSTEMD_CFLAGS)
f5gs_LDADD += \
	$(SYSTEMD_DAEMON_LIBS) \
	$(SYSTEMD_JOURNAL_LIBS) \
	$(SYSTEMD_LIBS)
endif

edit = sed \
	-e 's|@PACKAGE_BUGREPORT[@]|$(PACKAGE_BUGREPORT)|g' \
	-e 's|@PACKAGE_BUILD_DATE[@]|$(PACKAGE_BUILD_DATE)|g' \
	-e 's|@PACKAGE_MAINTAINER[@]|$(PACKAGE_MAINTAINER)|g' \
	-e 's|@PACKAGE_STRING[@]|$(PACKAGE_STRING)|g' \
	-e 's|@PACKAGE_URL[@]|$(PACKAGE_URL)|g' \
	-e 's|@PACKAGE_VERSION[@]|$(PACKAGE_VERSION)|g' \
	-e 's|@F5GS_TCP_PORT[@]|$(F5GS_TCP_PORT)|g' \
	-e 's|@bindir[@]|$(bindir)|g' \
	-e 's|@datadir[@]|$(datadir)|g' \
	-e 's|@f5gs_post[@]|$(f5gs_post)|g' \
	-e 's|@f5gs_pre[@]|$(f5gs_pre)|g' \
	-e 's|@f5gs_rundir[@]|$(f5gs_rundir)|g'

f5gs.8: f5gs.8.in
	$(edit) $(srcdir)/f5gs.8.in >$@
man_MANS = f5gs.8
CLEANFILES = f5gs.8

if HAVE_SYSTEMD
f5gs.service: contrib/f5gs.service.in
	@ rm -f $@ $@.tmp
	$(AM_V_GEN) srcdir=''; \
		test -f ./contrib/f5gs.service.in || srcdir=$(srcdir)/; \
		$(edit) $${srcdir}contrib/f5gs.service.in >$@.tmp
	@ mv $@.tmp $@
f5gs.socket: contrib/f5gs.socket.in
	@ rm -f $@ $@.tmp
	$(AM_V_GEN) srcdir=''; \
		test -f ./contrib/f5gs.socket.in || srcdir=$(srcdir)/; \
		$(edit) $${srcdir}contrib/f5gs.socket.in >$@.tmp
	@ mv $@.tmp $@
systemdsystemunit_DATA = f5gs.service f5gs.socket
CLEANFILES += f5gs.service f5gs.socket
endif

include tests/Makemodule.am

extrasdir = $(datadir)/@PACKAGE@
dist_extras_SCRIPTS = contrib/pre-script
dist_extras_DATA = ChangeLog

dist-hook: gen-ChangeLog
.PHONY: gen-ChangeLog
gen-ChangeLog:
	$(AM_V_GEN)if test -d .git; then			\
		git log --abbrev-commit --pretty=oneline --graph >| $(distdir)/cl-t &&	\
		{ rm -f $(distdir)/ChangeLog &&			\
		  mv $(distdir)/cl-t $(distdir)/ChangeLog ; }	\
	fi
