AUTOMAKE_OPTIONS = gnu
ACLOCAL_AMFLAGS = -I m4
AM_CPPFLAGS = -I$(top_srcdir)/lib -I$(top_builddir)/lib

EXTRA_DIST = m4/gnulib-cache.m4
SUBDIRS = lib

bin_PROGRAMS = f5gs
f5gs_SOURCES = f5gs.c
f5gs_LDADD = $(top_builddir)/lib/libf5gs.la $(LIB_PTHREAD)
