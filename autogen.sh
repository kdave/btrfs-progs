#!/bin/sh

#
# Helps generate autoconf stuff, when code is checked out from SCM.
#
# Copyright (C) 2006-2014 - Karel Zak <kzak@redhat.com>
#

srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

THEDIR=`pwd`
cd $srcdir
DIE=0

test -f btrfs.c || {
	echo
	echo "You must run this script in the top-level btrfs-progs directory"
	echo
	DIE=1
}

(autoconf --version) < /dev/null > /dev/null 2>&1 || {
	echo
	echo "You must have autoconf installed to generate btrfs-progs build system."
	echo
	DIE=1
}
(autoheader --version) < /dev/null > /dev/null 2>&1 || {
	echo
	echo "You must have autoheader installed to generate btrfs-progs build system."
	echo "The autoheader command is part of the GNU autoconf package."
	echo
	DIE=1
}

(automake --version) < /dev/null > /dev/null 2>&1 || {
	echo
	echo "You must have automake installed to generate btrfs-progs build system."
	echo
	DIE=1
}

(pkg-config --version) < /dev/null > /dev/null 2>&1 || {
	echo
	echo "You must have pkg-config installed to use btrfs-progs build system."
	echo "The pkg-config utility was not found in the standard location, set"
	echo "the PKG_CONFIG/PKG_CONFIG_PATH/PKG_CONFIG_LIBDIR variables at the"
	echo "configure time."
	echo
}

if test "$DIE" -eq 1; then
	exit 1
fi

echo
echo "Generate build-system by:"
echo "   aclocal:    $(aclocal --version | head -1)"
echo "   autoconf:   $(autoconf --version | head -1)"
echo "   autoheader: $(autoheader --version | head -1)"
echo "   automake:   $(automake --version | head -1)"

rm -rf autom4te.cache

aclocal -I m4 $AL_OPTS &&
autoconf -I m4 $AC_OPTS &&
autoheader -I m4 $AH_OPTS ||
exit 1

# it's better to use helper files from automake installation than
# maintain copies in git tree
find_autofile() {
	if [ -f "$1" ]; then
		return
	fi
	for HELPER_DIR in $(automake --print-libdir 2>/dev/null) \
			/usr/share/libtool \
			/usr/share/automake-* ; do
		f="$HELPER_DIR/$1"
		if [ -f "$f" ]; then
			cp "$f" config/
			return
		fi
	done
	echo "Cannot find "$1" in known locations"
	exit 1
}

mkdir -p config/
find_autofile config.guess
find_autofile config.sub
find_autofile install-sh

cd "$THEDIR"

echo
echo "Now type '$srcdir/configure' and 'make' to compile."
echo
