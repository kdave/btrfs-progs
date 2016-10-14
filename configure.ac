AC_INIT([btrfs-progs],
	m4_esyscmd([./version.sh --configure]),
	[linux-btrfs@vger.kernel.org],,
	[http://btrfs.wiki.kernel.org])

if test "x$PACKAGE_URL" = "x"; then
	AC_DEFINE([PACKAGE_URL], ["http://btrfs.wiki.kernel.org"], [URL])
fi

dnl library version
LIBBTRFS_MAJOR=0
LIBBTRFS_MINOR=1
LIBBTRFS_PATCHLEVEL=1

CFLAGS=${CFLAGS:-"-g -O1 -Wall -D_FORTIFY_SOURCE=2"}
AC_SUBST([CFLAGS])

AC_PREREQ([2.60])

AC_CONFIG_AUX_DIR([config])
dnl AC_USE_SYSTEM_EXTENSIONS must be called before any macros that run
dnl the compiler (like AC_PROG_LIBTOOL) to avoid autoconf errors.
AC_USE_SYSTEM_EXTENSIONS

AC_CONFIG_SRCDIR([btrfs.c])
AC_PREFIX_DEFAULT([/usr/local])

AC_PROG_CC
AC_CANONICAL_HOST
AC_C_CONST
AC_C_VOLATILE
AC_C_BIGENDIAN

AC_SYS_LARGEFILE

AC_PROG_INSTALL
AC_PROG_LN_S
AC_CHECK_TOOL([AR], [ar])
AC_PATH_PROG([RM], [rm], [rm])
AC_PATH_PROG([RMDIR], [rmdir], [rmdir])

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

if test "x$enable_backtrace" = xyes; then
	AC_CHECK_HEADERS([execinfo.h])
	AC_CHECK_FUNCS([backtrace backtrace_symbols_fd], [],
	      AC_MSG_ERROR([standard library does not have backtrace support]))
fi


AC_ARG_ENABLE([documentation],
	      AS_HELP_STRING([--disable-documentation], [do not build domumentation]),
  [], [enable_documentation=yes]
)
AS_IF([test "x$enable_documentation" = xyes], [DISABLE_DOCUMENTATION=0], [DISABLE_DOCUMENTATION=1])
AC_SUBST([DISABLE_DOCUMENTATION])

dnl detect tools to build documentation
if test "x$enable_documentation" = xyes; then
	AC_PATH_PROG([ASCIIDOC], [asciidoc], [asciidoc])
	AC_PATH_PROG([XMLTO], [xmlto], [xmlto])
	AC_PATH_PROG([GZIP], [gzip], [gzip])
	AC_PATH_PROG([MV], [mv], [mv])
	AC_PROG_SED
fi

AC_ARG_ENABLE([convert],
	      AS_HELP_STRING([--disable-convert], [do not build btrfs-convert]),
  [], [enable_convert=yes]
)

AS_IF([test "x$enable_convert" = xyes], [DISABLE_BTRFSCONVERT=0], [DISABLE_BTRFSCONVERT=1])
AC_SUBST([DISABLE_BTRFSCONVERT])

AC_ARG_WITH([convert],
	    AS_HELP_STRING([[[]--with-convert[[=auto]]]], [built-in filesystems for convert (default: auto)
supported (comma separated list): ext2]),
  [], [with_convert=auto]
)

if test "$with_convert" = "yes"; then
	with_convert=auto
fi

if test "$with_convert" = "no"; then
	with_convert=
fi

convertfs=
BTRFSCONVERT_EXT2=0
if test "x$enable_convert" = xyes; then
	if test "x$with_convert" = "xauto" || echo "$with_convert" | grep -q "ext2"; then
		PKG_CHECK_MODULES(EXT2FS, [ext2fs >= 1.42],,
			[PKG_CHECK_MODULES(EXT2FS, [ext2fs],
				[AC_DEFINE([HAVE_OLD_E2FSPROGS], [1],
					  [E2fsprogs does not support BIGALLOC])]
				)])
		PKG_CHECK_MODULES(COM_ERR, [com_err])
		convertfs="${convertfs:+$convertfs,}ext2"
		BTRFSCONVERT_EXT2=1
	fi
fi
AC_SUBST([BTRFSCONVERT_EXT2])

# catch typos
tmp=$(echo "$with_convert" | sed -e 's/auto//' | sed -e 's/ext2//' | sed -e 's/,\+/,/')
if ! test "x$tmp" = "x"; then
	AC_MSG_ERROR([unknown tokens for --with-convert: $tmp])
fi

if test "$DISABLE_BTRFSCONVERT" = 0 && test "x$convertfs" = "x"; then
	AC_MSG_ERROR([no filesystems for convert, use --disable-convert instead])
fi

HAVE_OWN_FIEMAP_EXTENT_SHARED_DEFINE=0
AX_CHECK_DEFINE([linux/fiemap.h], [FIEMAP_EXTENT_SHARED], [],
		[HAVE_OWN_FIEMAP_EXTENT_SHARED_DEFINE=1
		 AC_MSG_WARN([no definition of FIEMAP_EXTENT_SHARED found, probably old kernel, will use own defintion, 'btrfs fi du' might report wrong numbers])])

if test "x$HAVE_OWN_FIEMAP_EXTENT_SHARED_DEFINE" == "x1"; then
AC_DEFINE([HAVE_OWN_FIEMAP_EXTENT_SHARED_DEFINE], [1], [We defined FIEMAP_EXTENT_SHARED])
else
AC_DEFINE([HAVE_OWN_FIEMAP_EXTENT_SHARED_DEFINE], [0], [We did not define FIEMAP_EXTENT_SHARED])
fi

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

# udev v190 introduced the btrfs builtin and a udev rule to use it.
# Our udev rule gives us the friendly dm names but isn't required (or valid)
# on earlier releases.
UDEVDIR=
if pkg-config udev --atleast-version 190; then
	UDEVDIR="$(pkg-config udev --variable=udevdir)"
fi
AC_SUBST(UDEVDIR)

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
Documentation/Makefile
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

	documentation:     ${enable_documentation}
	backtrace support: ${enable_backtrace}
	btrfs-convert:     ${enable_convert} ${convertfs:+($convertfs)}

	Type 'make' to compile.
])

