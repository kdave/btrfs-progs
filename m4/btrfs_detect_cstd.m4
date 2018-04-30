dnl We prefer -std=gnu90 but gcc versions prior to 4.5.0 don't support
dnl it.  AX_CHECK_COMPILE_FLAG is the right way to determine whether a
dnl particular version of gcc supports a flag, but it requires autoconf
dnl 2.64.  Since (for now) we still want to support older releases
dnl that ship with autoconf 2.63, we the also-deprecated AX_GCC_VERSION
dnl macro there.
AC_DEFUN([BTRFS_DETECT_CSTD],
[
 m4_version_prereq([2.64], [
     AX_CHECK_COMPILE_FLAG([-std=gnu90],
       [BTRFS_CSTD_FLAGS=-std=gnu90],
       [BTRFS_CSTD_FLAGS=-std=gnu89])
   ], [
     AX_GCC_VERSION([4], [5], [0],
       [BTRFS_CSTD_FLAGS=-std=gnu90],
       [BTRFS_CSTD_FLAGS=-std=gnu89])
   ])
   AC_SUBST([BTRFS_CSTD_FLAGS])
]) dnl BTRFS_DETECT_CSTD

