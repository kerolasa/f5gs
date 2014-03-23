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
	contrib/init.redhat

SUBDIRS = lib

bin_PROGRAMS = f5gs
f5gs_SOURCES = f5gs.c f5gs.h
f5gs_LDADD = \
	$(top_builddir)/lib/lib$(PACKAGE).la \
	$(GETADDRINFO_LIB) \
	$(HOSTENT_LIB) \
	$(INET_NTOP_LIB) \
	$(LIBSOCKET) \
	$(LIB_POLL) \
	$(LIB_PTHREAD) \
	$(LIB_SELECT) \
	$(LTLIBINTL) \
	$(SERVENT_LIB)

if USE_SYSTEMD
f5gs_LDADD += \
	$(SYSTEMD_DAEMON_LIBS) \
	$(SYSTEMD_JOURNAL_LIBS)
endif

edit = sed \
	-e 's|@PACKAGE_BUGREPORT[@]|$(PACKAGE_BUGREPORT)|g' \
	-e 's|@PACKAGE_MAINTAINER[@]|$(PACKAGE_MAINTAINER)|g' \
	-e 's|@PACKAGE_STRING[@]|$(PACKAGE_STRING)|g' \
	-e 's|@PACKAGE_URL[@]|$(PACKAGE_URL)|g' \
	-e 's|@PACKAGE_VERSION[@]|$(PACKAGE_VERSION)|g' \
	-e 's|@PORT_NUM[@]|$(PORT_NUM)|g' \
	-e 's|@bindir[@]|$(bindir)|g' \
	-e 's|@datadir[@]|$(datadir)|g' \
	-e 's|@f5gs_post[@]|$(f5gs_post)|g' \
	-e 's|@f5gs_pre[@]|$(f5gs_pre)|g' \
	-e 's|@f5gs_rundir[@]|$(f5gs_rundir)|g'

f5gs.8: f5gs.8.in
	$(edit) $(srcdir)/f5gs.8.in >$@
man_MANS = f5gs.8
CLEANFILES = f5gs.8

if USE_SYSTEMD
f5gs.service: contrib/f5gs.service.in
	$(edit) contrib/f5gs.service.in >$@
f5gs.socket: contrib/f5gs.socket.in
	$(edit) contrib/f5gs.socket.in >$@
systemdsystemunit_DATA = f5gs.service f5gs.socket
CLEANFILES += f5gs.service f5gs.socket
endif

include tests/Makemodule.am

extrasdir = $(datadir)/@PACKAGE@
dist_extras_SCRIPTS = contrib/pre-script
