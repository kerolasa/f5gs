#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.63])
AC_CONFIG_MACRO_DIR([m4])
AC_INIT([f5gs],
	[m4_esyscmd([build-aux/git-version-gen .tarball-version])],
	[kerolasa@iki.fi], [],
	[https://github.com/kerolasa/f5gs])
PACKAGE_MAINTAINER="Sami Kerola"
AC_SUBST([PACKAGE_MAINTAINER])
PACKAGE_BUILD_DATE="$(date --iso)"
AC_SUBST([PACKAGE_BUILD_DATE])
AC_CONFIG_SRCDIR([f5gs.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([
	foreign
	1.11
	-Wall
	-Wextra-portability
	dist-xz
	no-dist-gzip
	subdir-objects
	tar-pax
	serial-tests
])

# Checks for programs.
AC_USE_SYSTEM_EXTENSIONS
AC_C_RESTRICT
AC_PROG_CC_C99
gl_EARLY
gl_INIT
LT_INIT

# Checks for libraries.
m4_ifndef([PKG_PROG_PKG_CONFIG],
  [m4_fatal([Could not locate the pkg-config autoconf
    macros. These are usually located in /usr/share/aclocal/pkg.m4.
    If your macros are in a different location, try setting the
    environment variable AL_OPTS="-I/other/macro/dir" before running
    ./autogen.sh or autoreconf again.])])
PKG_PROG_PKG_CONFIG

# Checks for header files.
AC_CHECK_HEADERS([arpa/inet.h fcntl.h netinet/in.h sys/socket.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_SSIZE_T

# Checks for library functions.
AC_FUNC_MMAP
AC_CHECK_FUNCS([ \
	accept4 \
	clearenv \
	epoll_create \
	epoll_create1
])

AC_ARG_WITH([systemd],
	AS_HELP_STRING([--with-systemd], [build with support for systemd]),
	[], [with_systemd=check]
)
have_systemd=no
AS_IF([test "x$with_systemd" != xno], [
	dnl systemd 209 or newer
	PKG_CHECK_MODULES([SYSTEMD], [libsystemd], [have_systemd=yes], [have_systemd=no])
	dnl old versions
	AS_IF([test "x$have_systemd" != "xyes"], [
		PKG_CHECK_MODULES([SYSTEMD_DAEMON], [libsystemd-daemon],
			[have_systemd_daemon=yes], [have_systemd_daemon=no])
		PKG_CHECK_MODULES([SYSTEMD_JOURNAL], [libsystemd-journal],
			[have_systemd_journal=yes], [have_systemd_journal=no])
		AS_IF([test "x$have_systemd_daemon" = "xyes" -a "x$have_systemd_journal" = "xyes"],
			[have_systemd=yes])
	])
	AS_CASE([$with_systemd:$have_systemd],
	[yes:no],
		[AC_MSG_ERROR([systemd expected but libsystemd not found])],
	[*:yes],
		AC_DEFINE([HAVE_LIBSYSTEMD], [1], [libsystemd is available])
	)
])
AM_CONDITIONAL([HAVE_SYSTEMD], [test "x$have_systemd" = xyes])

AC_ARG_WITH([systemdsystemunitdir],
	AS_HELP_STRING([--with-systemdsystemunitdir=DIR], [directory for systemd service files]),
	[], [with_systemdsystemunitdir=$prefix/lib/systemd/system])
AS_IF([test "x$with_systemdsystemunitdir" != "xno"], [
	AC_SUBST([systemdsystemunitdir], [$with_systemdsystemunitdir])
])

AC_DEFUN([DEFAULT_PORT], [32546])
AC_ARG_WITH([default-portl],
	[AS_HELP_STRING([--with-default-port=NUM], [default listen port number (@<:@DEFAULT_PORT@:>@)])],
	[default_port="$withval"],
	[default_port="DEFAULT_PORT"])
AC_DEFINE_UNQUOTED([F5GS_TCP_PORT], ["$default_port"], [default listen port number])
AC_SUBST([F5GS_TCP_PORT], [$default_port])

AC_SUBST([f5gs_post], [$sysconfdir/$PACKAGE/post], [f5gs state chante post-script])
AC_SUBST([f5gs_pre], [$sysconfdir/$PACKAGE/pre], [f5gs state change pre-script])
AC_SUBST([f5gs_rundir], [$localstatedir/lib/$PACKAGE/], [f5gs runtime state directory])

AC_CONFIG_FILES([Makefile
		lib/Makefile
])
AC_OUTPUT
