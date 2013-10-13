#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_CONFIG_MACRO_DIR([m4])
AC_INIT([f5gs],
	[m4_esyscmd([build-aux/git-version-gen .tarball-version])],
	[kerolasa@iki.fi], [],
	[https://github.com/kerolasa/f5gs])
PACKAGE_MAINTAINER="Sami Kerola"
AC_SUBST([PACKAGE_MAINTAINER])
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
])

# Checks for programs.
AC_USE_SYSTEM_EXTENSIONS
AC_C_RESTRICT
AC_PROG_CC
gl_EARLY
gl_INIT
LT_INIT

# Checks for libraries.

# Checks for header files.
AC_CHECK_HEADERS([arpa/inet.h fcntl.h netinet/in.h sys/socket.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_SSIZE_T

# Checks for library functions.
AC_FUNC_MMAP
AC_CHECK_FUNCS([munmap socket])

AC_DEFUN([DEFAULT_PORT], [32546])
AC_ARG_WITH([default-portl],
	[AS_HELP_STRING([--with-default-port=NUM], [default port (@<:@DEFAULT_PORT@:>@)])],
	[default_port="$withval"],
	[default_port="DEFAULT_PORT"])
AC_DEFINE_UNQUOTED([PORT_NUM], ["$default_port"], [default port])
AC_SUBST([PORT_NUM], [$default_port])

AC_SUBST([f5gs_rundir], [$runstatedir/$PACKAGE/], [f5gs runtime state directory])

AC_CONFIG_FILES([Makefile
		lib/Makefile
])
AC_OUTPUT
