AC_INIT([btrfs-progs],
	m4_flatten(m4_include([VERSION])),
	[linux-btrfs@vger.kernel.org],,
	[https://btrfs.readthedocs.io])

if test "x$PACKAGE_URL" = "x"; then
	AC_DEFINE([PACKAGE_URL], ["https://btrfs.readthedocs.io"], [URL])
fi

dnl Package version
BTRFS_VERSION_PLAIN=`cat VERSION`
BTRFS_VERSION_PLAIN="${BTRFS_VERSION_PLAIN#v*}"
BTRFS_VERSION_MAJOR=`echo "${BTRFS_VERSION_PLAIN#v*}" | awk -F. '{print $1}'`
BTRFS_VERSION_MINOR=`echo "${BTRFS_VERSION_PLAIN#v*}" | awk -F. '{print $2}'`
BTRFS_VERSION_PATCHLEVEL=`echo "${BTRFS_VERSION_PLAIN#v*}" | awk -F. '{print $3}'`

dnl libbtrfs .so version
LIBBTRFS_MAJOR=0
LIBBTRFS_MINOR=1
LIBBTRFS_PATCHLEVEL=4

dnl libbtrfsutil .so version
BTRFS_UTIL_VERSION_MAJOR=`sed -rn 's/^\#define BTRFS_UTIL_VERSION_MAJOR ([0-9])+$/\1/p' libbtrfsutil/btrfsutil.h`
BTRFS_UTIL_VERSION_MINOR=`sed -rn 's/^\#define BTRFS_UTIL_VERSION_MINOR ([0-9])+$/\1/p' libbtrfsutil/btrfsutil.h`
BTRFS_UTIL_VERSION_PATCH=`sed -rn 's/^\#define BTRFS_UTIL_VERSION_PATCH ([0-9])+$/\1/p' libbtrfsutil/btrfsutil.h`

CFLAGS=${CFLAGS:-"-g -O2 -Wall -D_FORTIFY_SOURCE=2"}
AC_SUBST([CFLAGS])

AC_PREREQ([2.60])

AC_CONFIG_AUX_DIR([config])
AC_CONFIG_MACRO_DIR([config])
dnl AC_USE_SYSTEM_EXTENSIONS must be called before any macros that run
dnl the compiler (like AC_PROG_LIBTOOL) to avoid autoconf errors.
AC_USE_SYSTEM_EXTENSIONS

AC_CONFIG_SRCDIR([btrfs.c])
AC_PREFIX_DEFAULT([/usr/local])

AC_PROG_CC
AC_CANONICAL_HOST
AC_CANONICAL_TARGET
AC_C_CONST
AC_C_VOLATILE
AC_C_BIGENDIAN

AS_IF([test "x$target_cpu" = 'xx86_64'], [
	AX_CHECK_COMPILE_FLAG([-msse2], [HAVE_CFLAG_msse2=1], [HAVE_CFLAG_msse2=0])
	AC_SUBST([HAVE_CFLAG_msse2])
	AC_DEFINE_UNQUOTED([HAVE_CFLAG_msse2], [$HAVE_CFLAG_msse2], [Compiler supports -msse2])

	AX_CHECK_COMPILE_FLAG([-msse4.1], [HAVE_CFLAG_msse41=1], [HAVE_CFLAG_msse41=0])
	AC_SUBST([HAVE_CFLAG_msse41])
	AC_DEFINE_UNQUOTED([HAVE_CFLAG_msse41], [$HAVE_CFLAG_msse41], [Compiler supports -msse4.1])

	AX_CHECK_COMPILE_FLAG([-mavx2], [HAVE_CFLAG_mavx2=1], [HAVE_CFLAG_mavx2=0])
	AC_SUBST([HAVE_CFLAG_mavx2])
	AC_DEFINE_UNQUOTED([HAVE_CFLAG_mavx2], [$HAVE_CFLAG_mavx2], [Compiler supports -mavx2])

	AX_CHECK_COMPILE_FLAG([-msha], [HAVE_CFLAG_msha=1], [HAVE_CFLAG_msha=0])
	AC_SUBST([HAVE_CFLAG_msha])
	AC_DEFINE_UNQUOTED([HAVE_CFLAG_msha], [$HAVE_CFLAG_msha], [Compiler supports -msha])
], [])

AX_CHECK_COMPILE_FLAG([-std=gnu11], [],
	[AC_MSG_ERROR([std=gnu11 compiler required])])

AC_SYS_LARGEFILE

AC_PROG_INSTALL
AC_PROG_LN_S
AC_CHECK_TOOL([AR], [ar])
AC_PATH_PROG([RM], [rm], [rm])
AC_PATH_PROG([RMDIR], [rmdir], [rmdir])

AX_CHECK_DEFINE([features.h],[__GLIBC__],[HAVE_GLIBC=1],[HAVE_GLIBC=0])
AC_SUBST([HAVE_GLIBC])

AX_GCC_BUILTIN([__builtin_add_overflow])
AX_GCC_BUILTIN([__builtin_sub_overflow])
AX_GCC_BUILTIN([__builtin_mul_overflow])
AX_GCC_BUILTIN([__builtin_cpu_supports__pclmul])

AC_CHECK_HEADERS([fcntl.h])
AC_CHECK_HEADERS([inttypes.h])
AC_CHECK_HEADERS([malloc.h])
AC_CHECK_HEADERS([mntent.h])
AC_CHECK_HEADERS([stdint.h])
AC_CHECK_HEADERS([strings.h])
AC_CHECK_HEADERS([sys/file.h])
AC_CHECK_HEADERS([sys/ioctl.h])
AC_CHECK_HEADERS([sys/param.h])
AC_CHECK_HEADERS([sys/socket.h])
AC_CHECK_HEADERS([sys/statfs.h])
AC_CHECK_HEADERS([sys/statvfs.h])
AC_CHECK_HEADERS([sys/time.h])
AC_CHECK_HEADERS([unistd.h])
AC_CHECK_HEADER_STDBOOL

AC_CHECK_HEADERS([linux/perf_event.h])
AC_CHECK_HEADERS([linux/hw_breakpoint.h])
AC_CHECK_HEADERS([linux/fsverity.h])

AC_CHECK_MEMBERS([struct stat.st_blksize])
AC_CHECK_MEMBERS([struct stat.st_rdev])

AC_TYPE_INT32_T
AC_TYPE_INT64_T
AC_TYPE_INT8_T
AC_TYPE_MODE_T
AC_TYPE_OFF_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UID_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T

dnl Required functions
AC_FUNC_CHOWN
AC_FUNC_ERROR_AT_LINE
AC_FUNC_FORK
AC_FUNC_GETMNTENT
AC_FUNC_LSTAT_FOLLOWS_SLASHED_SYMLINK
AC_FUNC_MALLOC
AC_FUNC_MMAP
AC_FUNC_REALLOC

AC_CHECK_FUNCS([fchdir], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([fdatasync], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([ftruncate], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([getcwd], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([getmntent], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([getpagesize], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([gettimeofday], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([lchown], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([localtime_r], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([memchr], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([memmove], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([memset], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([mkdir], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([mkfifo], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([munmap], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([openat], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([realpath], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([regcomp], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([rmdir], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([select], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([socket], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([strcasecmp], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([strchr], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([strdup], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([strerror], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([strncasecmp], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([strndup], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([strrchr], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([strstr], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([strtol], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([strtoul], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([strtoull], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([sysinfo], [], [AC_MSG_ERROR([cannot find required function])])
AC_CHECK_FUNCS([uname], [], [AC_MSG_ERROR([cannot find required function])])

dnl Check for printf.h functions.
AC_CHECK_HEADERS([printf.h])
AC_CHECK_FUNCS([register_printf_specifier])
AC_CHECK_FUNCS([register_printf_modifier])

dnl Optional (tests)
AC_CHECK_FUNCS([clock_gettime])

if grep -q 'HAVE_LINUX_FSVERITY_H.*1' confdefs.h; then
	have_fsverity='yes'
else
	have_fsverity='no'
fi

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
  if AC_RUN_LOG([${PKG_CONFIG} --exists --print-errors "$2"]); then
    $1=`${PKG_CONFIG} --libs --static "$2"`
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
	      AS_HELP_STRING([--disable-documentation], [do not build documentation]),
  [], [enable_documentation=yes]
)
AS_IF([test "x$enable_documentation" = xyes], [DISABLE_DOCUMENTATION=0], [DISABLE_DOCUMENTATION=1])
AC_SUBST([DISABLE_DOCUMENTATION])

dnl detect tools to build documentation
DOC_TOOL="none"
if test "x$enable_documentation" = xyes; then
	DOC_TOOL='sphinx'
	AC_PATH_PROG([SPHINX], [sphinx-build])
	AC_PROG_SED

	if test -z "$SPHINX"; then
		AC_MSG_ERROR([cannot find sphinx-build, cannot build documentation])
	fi
fi
AC_SUBST([DOC_TOOL])

AC_ARG_ENABLE([programs],
	      AS_HELP_STRING([--disable-programs], [do not build utility programs]),
	      [], [enable_programs=yes]
)
AS_IF([test "x$enable_programs" = xyes], [BUILD_PROGRAMS=1], [BUILD_PROGRAMS=0])
AC_SUBST([BUILD_PROGRAMS])

AC_ARG_ENABLE([shared],
	      AS_HELP_STRING([--disable-shared], [do not build shared libraries]),
	      [], [enable_shared=yes]
)
AS_IF([test "x$enable_shared" = xyes], [BUILD_SHARED_LIBRARIES=1], [BUILD_SHARED_LIBRARIES=0])
AC_SUBST([BUILD_SHARED_LIBRARIES])

AC_ARG_ENABLE([static],
	      AS_HELP_STRING([--disable-static], [do not build static libraries]),
	      [], [enable_static=yes]
)
AS_IF([test "x$enable_static" = xyes], [BUILD_STATIC_LIBRARIES=1], [BUILD_STATIC_LIBRARIES=0])
AC_SUBST([BUILD_STATIC_LIBRARIES])

AC_ARG_ENABLE([convert],
	      AS_HELP_STRING([--disable-convert], [do not build btrfs-convert]),
  [], [enable_convert=$enable_programs]
)

AS_IF([test "x$enable_convert" = xyes], [DISABLE_BTRFSCONVERT=0], [DISABLE_BTRFSCONVERT=1])
AC_SUBST([DISABLE_BTRFSCONVERT])

AC_ARG_WITH([convert],
	    AS_HELP_STRING([[[]--with-convert[[=auto]]]], [built-in filesystems for convert (default: auto)
supported (comma separated list): ext2,reiserfs]),
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
BTRFSCONVERT_REISERFS=0
if test "x$enable_convert" = xyes; then
	if test "x$with_convert" = "xauto" || echo "$with_convert" | grep -q "ext2"; then
		PKG_CHECK_MODULES(EXT2FS, [ext2fs])
		PKG_CHECK_MODULES(COM_ERR, [com_err])
		convertfs="${convertfs:+$convertfs,}ext2"
		BTRFSCONVERT_EXT2=1
	fi
	if test "x$with_convert" = "xauto"; then
		PKG_CHECK_MODULES(REISERFS, [reiserfscore >= 3.6.27],
				  [BTRFSCONVERT_REISERFS=1],
				  [BTRFSCONVERT_REISERFS=0])
	elif echo "$with_convert" | grep -q "reiserfs"; then
		PKG_CHECK_MODULES(REISERFS, [reiserfscore >= 3.6.27],
				  [BTRFSCONVERT_REISERFS=1],[])
	fi
	if test "$BTRFSCONVERT_REISERFS" = 1; then
		convertfs="${convertfs:+$convertfs,}reiserfs"
	fi
fi
AC_SUBST([BTRFSCONVERT_EXT2])
AC_SUBST([BTRFSCONVERT_REISERFS])

# catch typos
tmp=$(echo "$with_convert" | sed -e 's/auto//' | sed -e 's/ext2//' | sed -e 's/reiserfs//' | sed -e 's/,\+//')
if ! test "x$tmp" = "x"; then
	AC_MSG_ERROR([unknown tokens for --with-convert: $tmp])
fi

if test "$DISABLE_BTRFSCONVERT" = 0 && test "x$convertfs" = "x"; then
	AC_MSG_ERROR([no filesystems for convert, use --disable-convert instead])
fi

AC_ARG_WITH([crypto],
	    AS_HELP_STRING([[[]--with-crypto[[=builtin]]]], [provider of cryptographic primitives: builtin, libgcrypt, libsodium, libkcapi, botan, openssl]),
  [], [with_crypto=builtin]
)

cryptoprovider=
cryptoproviderversion=
CRYPTOPROVIDER_BUILTIN=0
CRYPTOPROVIDER_LIBGCRYPT=0
CRYPTOPROVIDER_LIBSODIUM=0
CRYPTOPROVIDER_LIBKCAPI=0
CRYPTOPROVIDER_BOTAN=0
CRYPTOPROVIDER_OPENSSL=0
if test "$with_crypto" = "builtin"; then
	cryptoprovider="builtin"
	CRYPTOPROVIDER_BUILTIN=1
elif test "$with_crypto" = "libgcrypt"; then
	cryptoprovider="libgcrypt"
	PKG_CHECK_MODULES(GCRYPT, [libgcrypt >= 1.8.0])
	CRYPTOPROVIDER_LIBGCRYPT=1
	cryptoproviderversion=`${PKG_CONFIG} libgcrypt --modversion`
elif test "$with_crypto" = "libsodium"; then
	cryptoprovider="libsodium"
	PKG_CHECK_MODULES(SODIUM, [libsodium >= 1.0.4])
	CRYPTOPROVIDER_LIBSODIUM=1
	cryptoproviderversion=`${PKG_CONFIG} libsodium --modversion`
elif test "$with_crypto" = "libkcapi"; then
	cryptoprovider="libkcapi"
	PKG_CHECK_MODULES(KCAPI, [libkcapi >= 1.0.0])
	CRYPTOPROVIDER_LIBKCAPI=1
	cryptoproviderversion=`${PKG_CONFIG} libkcapi --modversion`
elif test "$with_crypto" = "botan"; then
	cryptoprovider="botan"
	PKG_CHECK_MODULES(BOTAN, [botan-3 >= 3.0.0])
	CRYPTOPROVIDER_BOTAN=1
	cryptoproviderversion=`${PKG_CONFIG} botan-3 --modversion`
elif test "$with_crypto" = "openssl"; then
	cryptoprovider="openssl"
	PKG_CHECK_MODULES(OPENSSL, [libcrypto >= 3.2.0])
	CRYPTOPROVIDER_OPENSSL=1
	cryptoproviderversion=`${PKG_CONFIG} libcrypto --modversion`
else
	AC_MSG_ERROR([unrecognized crypto provider: $with_crypto])
fi
AC_SUBST([CRYPTOPROVIDER_BUILTIN])
AC_DEFINE_UNQUOTED([CRYPTOPROVIDER_BUILTIN],[$CRYPTOPROVIDER_BUILTIN],[Use builtin implementation])
AC_SUBST([CRYPTOPROVIDER_LIBGCRYPT])
AC_DEFINE_UNQUOTED([CRYPTOPROVIDER_LIBGCRYPT],[$CRYPTOPROVIDER_LIBGCRYPT],[Use libcrypt])
AC_SUBST([CRYPTOPROVIDER_LIBSODIUM])
AC_DEFINE_UNQUOTED([CRYPTOPROVIDER_LIBSODIUM],[$CRYPTOPROVIDER_LIBSODIUM],[Use libsodium])
AC_SUBST([CRYPTOPROVIDER_LIBKCAPI])
AC_DEFINE_UNQUOTED([CRYPTOPROVIDER_LIBKCAPI],[$CRYPTOPROVIDER_LIBKCAPI],[Use libkcapi])
AC_SUBST([CRYPTOPROVIDER_BOTAN])
AC_DEFINE_UNQUOTED([CRYPTOPROVIDER_BOTAN],[$CRYPTOPROVIDER_BOTAN],[Use Botan])
AC_SUBST([CRYPTOPROVIDER_OPENSSL])
AC_DEFINE_UNQUOTED([CRYPTOPROVIDER_OPENSSL],[$CRYPTOPROVIDER_OPENSSL],[Use OpenSSL])
AC_DEFINE_UNQUOTED([CRYPTOPROVIDER],["$cryptoprovider"],[Crypto implementation source name])

AX_CHECK_DEFINE([linux/fiemap.h], [FIEMAP_EXTENT_SHARED], [],
		[AC_DEFINE([HAVE_OWN_FIEMAP_EXTENT_SHARED_DEFINE], [1],
                           [Define to 1 if kernel headers do not define FIEMAP_EXTENT_SHARED])
		 AC_MSG_WARN([no definition of FIEMAP_EXTENT_SHARED found, probably old kernel, will use own definition, 'btrfs fi du' might report wrong numbers])])

AX_CHECK_DEFINE([ext2fs/ext2_fs.h], [EXT4_EPOCH_MASK],
		[AC_DEFINE([HAVE_EXT4_EPOCH_MASK_DEFINE], [1],
			   [Define to 1 if e2fsprogs defines EXT4_EPOCH_MASK])],
        [have_ext4_epoch_mask_define=no])

AS_IF([test "x$have_ext4_epoch_mask_define" = xno], [
    AC_CHECK_MEMBERS([struct ext2_inode_large.i_atime_extra],
        [
            AC_DEFINE([HAVE_EXT4_EPOCH_MASK_DEFINE], [1], [Define to 1 if ext2_inode_large includes i_atime_extra])
            AC_DEFINE([EXT4_EPOCH_BITS], [2],[for encode and decode tv_nsec in ext2 inode])
            AC_DEFINE([EXT4_EPOCH_MASK], [((1U << EXT4_EPOCH_BITS) - 1)], [For encode and decode tv_nsec info in ext2 inode])
            AC_DEFINE([EXT4_NSEC_MASK],  [(~0UL << EXT4_EPOCH_BITS)], [For encode and decode tv_nsec info in ext2 inode])
	    # Use name from 1.47.1, backward compatibility is handled in convert/source-ext2.c
            AC_DEFINE([ext2fs_inode_includes(size, field)],[m4_normalize[(size >= (sizeof(((struct ext2_inode_large *)0)->field) + offsetof(struct ext2_inode_large, field)))]],
                [For encode and decode tv_nsec info in ext2 inode])
        ],
        [AC_MSG_WARN([It seems that ext2_inode_large don't includes tv_nsec related info, probably old e2fsprogs, no 64bit time precision of converted images])],
        [[#include <ext2fs/ext2_fs.h>]])
])

AC_CHECK_HEADER(linux/blkzoned.h, [blkzoned_found=yes], [blkzoned_found=no])
AC_CHECK_MEMBER([struct blk_zone.capacity], [blkzoned_capacity=yes], [blkzoned_capacity=no], [[#include <linux/blkzoned.h>]])
AX_CHECK_DEFINE([linux/blkzoned.h], [BLKGETZONESZ], [blkzoned_getzonesz=yes], [blkzoned_getzonesz=no])
AC_ARG_ENABLE([zoned],
	      AS_HELP_STRING([--disable-zoned], [disable zoned block device support (default: detect)]),
	      [], [enable_zoned=$blkzoned_capacity]
)

# Autodetect zoned support
AS_IF([test "x$enableval" = "x"], [
       cansupportzoned=yes
       if test "x$blkzoned_found" = xno; then
	       cansupportzoned=no
       fi
       if test "x$blkzoned_capacity" = xno; then
	       cansupportzoned=no
       fi
       if test "x$blkzoned_getzonesz" = xno; then
	       cansupportzoned=no
       fi
       if test "x$cansupportzoned" = xno; then
	       AC_MSG_NOTICE([insufficient support for zoned mode, cannot enable])
	       enable_zoned=no
       else
	       enable_zoned=yes
       fi
])

# Explicitly requested by --enable-zoned
AS_IF([test "x$enable_zoned" = xyes], [
       if test "x$blkzoned_found" = xno; then
	       AC_MSG_ERROR([Couldn't find linux/blkzoned.h])
       fi
       if test "x$blkzoned_capacity" = xno; then
	       AC_MSG_ERROR([linux/blkzoned.h does not provide blk_zone.capacity])
       fi
       if test "x$blkzoned_getzonesz" = xno; then
	       AC_MSG_ERROR([linux/blkzoned.h does not define BLKGETZONESZ])
       fi
       AC_DEFINE([BTRFS_ZONED], [1], [Define to 1 if zoned device support is available])
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

AC_ARG_ENABLE([zstd],
	AS_HELP_STRING([--disable-zstd], [build without zstd compression support (btrfs-restore, btrfs-receive, mkfs.btrfs) (default: enabled)]),
	[], [enable_zstd=yes]
)

if test "x$enable_zstd" = xyes; then
	PKG_CHECK_MODULES(ZSTD, [libzstd >= 1.0.0])
	PKG_STATIC(ZSTD_LIBS_STATIC, [libzstd])
fi

AS_IF([test "x$enable_zstd" = xyes], [COMPRESSION_ZSTD=1], [COMPRESSION_ZSTD=0])
AC_SUBST(COMPRESSION_ZSTD)

AC_ARG_ENABLE([lzo],
	AS_HELP_STRING([--disable-lzo], [build without lzo compression support (btrfs-restore, btrfs-receive, mkfs.btrfs) (default: enabled)]),
	[], [enable_lzo=yes]
)

if test "x$enable_lzo" = xyes; then
	dnl lzo library does not provide pkg-config, use classic way
	AC_CHECK_LIB([lzo2], [lzo_version], [
		LZO2_LIBS="-llzo2"
		LZO2_CFLAGS=""
		LZO2_LIBS_STATIC="-llzo2"],[
		AC_MSG_ERROR([cannot find lzo2 library])
	])
	AC_SUBST([LZO2_LIBS])
	AC_SUBST([LZO2_LIBS_STATIC])
	AC_SUBST([LZO2_CFLAGS])
fi

AS_IF([test "x$enable_lzo" = xyes], [COMPRESSION_LZO=1], [COMPRESSION_LZO=0])
AC_SUBST(COMPRESSION_LZO)

AC_ARG_ENABLE([libudev],
      AS_HELP_STRING([--disable-libudev], [build without libudev support (for multipath)]),
      [], [enable_libudev=yes]
)

if test "x$enable_libudev" = xyes; then
	PKG_CHECK_MODULES([LIBUDEV], [libudev])
	AC_DEFINE([HAVE_LIBUDEV], [1], [Define to 1 if libudev is available])
fi

AC_ARG_ENABLE([python],
	AS_HELP_STRING([--disable-python], [do not build libbtrfsutil Python bindings]),
	[], [enable_python=$enable_shared]
)

if test "x$enable_python" = xyes; then
	AM_PATH_PYTHON([3.4])
	PKG_CHECK_MODULES(PYTHON, [python-${PYTHON_VERSION}])
fi

AS_IF([test "x$enable_python" = xyes], [PYTHON_BINDINGS=1], [PYTHON_BINDINGS=0])
AC_SUBST(PYTHON_BINDINGS)
AC_SUBST(PYTHON)

AC_ARG_ENABLE([experimental],
	AS_HELP_STRING([--enable-experimental], [allow UNSTABLE and EXPERIMENTAL features]))
AS_IF([test "x$enable_experimental" = xyes], [EXPERIMENTAL=1], [EXPERIMENTAL=0])
AC_SUBST(EXPERIMENTAL)
AC_DEFINE_UNQUOTED([EXPERIMENTAL],[$EXPERIMENTAL],[Define to 1 if you experimental and unstable features are build])

experimental_msg=
if test "x$EXPERIMENTAL" = x1; then
	experimental_msg="
	WARNING: experimental and unstable features are enabled, do not use this
	         build for production and please report issues
"
fi

# udev v190 introduced the btrfs builtin and a udev rule to use it.
# Our udev rule gives us the friendly dm names but isn't required (or valid)
# on earlier releases.
UDEVDIR=
if ${PKG_CONFIG} udev --atleast-version 190; then
	UDEVDIR="$(${PKG_CONFIG} udev --variable=udevdir)"
fi
AC_SUBST(UDEVDIR)

dnl call PKG_INSTALLDIR from pkg.m4 to set pkgconfigdir
m4_ifdef([PKG_INSTALLDIR], [PKG_INSTALLDIR], [AC_MSG_ERROR([please install pkgconf])])

dnl Versions
AC_SUBST([BTRFS_VERSION_PLAIN])
AC_SUBST([BTRFS_VERSION_MAJOR])
AC_SUBST([BTRFS_VERSION_MINOR])
AC_SUBST([BTRFS_VERSION_PATCHLEVEL])

AC_SUBST([LIBBTRFS_MAJOR])
AC_SUBST([LIBBTRFS_MINOR])
AC_SUBST([LIBBTRFS_PATCHLEVEL])

AC_SUBST([BTRFS_UTIL_VERSION_MAJOR])
AC_SUBST([BTRFS_UTIL_VERSION_MINOR])
AC_SUBST([BTRFS_UTIL_VERSION_PATCH])

AC_CONFIG_HEADERS([include/config.h])

AC_CONFIG_FILES([
Makefile.inc
Documentation/Makefile
libbtrfs/version.h
libbtrfsutil/libbtrfsutil.pc
libbtrfsutil/python/version.py
])

AC_OUTPUT

AC_MSG_RESULT([
	btrfs-progs:        ${PACKAGE_VERSION}
	libbtrfs:           ${LIBBTRFS_MAJOR}.${LIBBTRFS_MINOR}.${LIBBTRFS_PATCHLEVEL} (deprecated)
	libbtrfsutil:       ${BTRFS_UTIL_VERSION_MAJOR}.${BTRFS_UTIL_VERSION_MINOR}.${BTRFS_UTIL_VERSION_PATCH}

	prefix:             ${prefix}
	exec prefix:        ${exec_prefix}

	bindir:             ${bindir}
	libdir:             ${libdir}
	includedir:         ${includedir}
	pkgconfigdir:       ${pkgconfigdir}

	compiler:           ${CC}
	CFLAGS:             ${CFLAGS}
	LDFLAGS:            ${LDFLAGS}

	programs:           ${enable_programs}
	shared libraries:   ${enable_shared}
	static libraries:   ${enable_static}
	documentation:      ${enable_documentation}
	doc generator:      ${DOC_TOOL}
	backtrace support:  ${enable_backtrace}
	btrfs-convert:      ${enable_convert} ${convertfs:+($convertfs)}
	zstd support:       ${enable_zstd}
	lzo support:        ${enable_lzo}
	fsverity support:   ${have_fsverity}
	Python bindings:    ${enable_python}
	Python interpreter: ${PYTHON}
	crypto provider:    ${cryptoprovider} ${cryptoproviderversion}
	libudev:            ${enable_libudev}
	zoned device:       ${enable_zoned}
	${experimental_msg}
	Type 'make' to compile.
])
