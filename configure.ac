AC_INIT([btrfs-progs],
	m4_esyscmd([./version.sh --configure]),
	[linux-btrfs@vger.kernel.org],,
	[http://btrfs.wiki.kernel.org])

dnl library version
LIBBTRFS_MAJOR=0
LIBBTRFS_MINOR=1
LIBBTRFS_PATCHLEVEL=1

CFLAGS="-g -O1"

AC_PREREQ([2.60])

AC_CONFIG_AUX_DIR([config])
dnl AC_USE_SYSTEM_EXTENSIONS must be called before any macros that run
dnl the compiler (like AC_PROG_LIBTOOL) to avoid autoconf errors.
AC_USE_SYSTEM_EXTENSIONS

AC_CONFIG_SRCDIR([btrfs.c])
AC_PREFIX_DEFAULT([/usr])

AC_PROG_CC
AC_CANONICAL_HOST
AC_C_CONST
AC_C_VOLATILE
AC_C_BIGENDIAN

AC_SYS_LARGEFILE

AC_PROG_INSTALL
AC_PROG_LN_S
AC_PATH_PROG([AR], [ar])

AC_CHECK_FUNCS([openat], [],
	[AC_MSG_ERROR([cannot find openat() function])])

m4_ifndef([PKG_PROG_PKG_CONFIG],
  [m4_fatal([Could not locate the pkg-config autoconf
    macros. These are usually located in /usr/share/aclocal/pkg.m4.
    If your macros are in a different location, try setting the
    environment variable AL_OPTS="-I/other/macro/dir" before running
    ./autogen.sh or autoreconf again.])])
PKG_PROG_PKG_CONFIG


dnl
dnl Calls pkg-config --static
dnl
AC_DEFUN([PKG_STATIC], [
  if AC_RUN_LOG([pkg-config --exists --print-errors "$2"]); then
    $1=`pkg-config --libs --static "$2"`
    AC_SUBST([$1])
  else
    AC_MSG_ERROR([pkg-config description of $2, needed for static build, is not available])
  fi
])


AC_ARG_ENABLE([backtrace],
  AS_HELP_STRING([--disable-backtrace], [disable btrfs backtrace]),
  [], [enable_backtrace=yes]
)

AS_IF([test "x$enable_backtrace" = xno], [
  AC_DEFINE([BTRFS_DISABLE_BACKTRACE], [1], [disable backtrace stuff in kerncompat.h ])
])

dnl Define <NAME>_LIBS= and <NAME>_CFLAGS= by pkg-config
dnl
dnl The default PKG_CHECK_MODULES() action-if-not-found is end the
dnl execution with error. The static libs are optional.

PKG_CHECK_MODULES(BLKID, [blkid])
PKG_STATIC(BLKID_LIBS_STATIC, [blkid])

PKG_CHECK_MODULES(UUID, [uuid])
PKG_STATIC(UUID_LIBS_STATIC, [uuid])

PKG_CHECK_MODULES(ZLIB, [zlib])
PKG_STATIC(ZLIB_LIBS_STATIC, [zlib])

dnl lzo library does not provide pkg-config, let use classic way
AC_CHECK_LIB([lzo2], [lzo_version], [
	LZO2_LIBS="-llzo2"
	LZO2_CFLAGS=""
	LZO2_LIBS_STATIC="-llzo2"],[
	AC_MSG_ERROR([cannot find lzo2 library])
])
AC_SUBST([LZO2_LIBS])
AC_SUBST([LZO2_LIBS_STATIC])
AC_SUBST([LZO2_CFLAGS])


dnl library stuff
AC_SUBST([LIBBTRFS_MAJOR])
AC_SUBST([LIBBTRFS_MINOR])
AC_SUBST([LIBBTRFS_PATCHLEVEL])

AC_CONFIG_HEADERS([config.h])

AC_CONFIG_FILES([
Makefile
version.h
])

AC_OUTPUT

AC_MSG_RESULT([
	${PACKAGE_NAME} ${PACKAGE_VERSION}

	prefix:            ${prefix}
	exec prefix:       ${exec_prefix}

	bindir:            ${bindir}
	libdir:            ${libdir}
	includedir:        ${includedir}

	compiler:          ${CC}
	cflags:            ${CFLAGS}
	ldflags:           ${LDFLAGS}

	Type 'make' to compile.
])

