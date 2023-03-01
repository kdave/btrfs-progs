#!/bin/sh
# Usage: $0 [--ccache] [make options]
#
# Test various compilation options:
# - native arch
# - dynamic, static
# - various configure options
#
# Arguments:
# - (first arugment) --ccache - enable ccache for build which can speed up
#    rebuilding same files if the options do not affect them, the ccache will
#    be created in the toplevel git directory
# - anything else will be passed to 'make', eg. define CC, D, V
#
# Requirements for full coverage:
# - static version of all libs

die() {
	echo "ERROR: $@"
	exit 1
}

check_result() {
	local ret
	local str

	ret=$1

	str="RESULT of target($target) conf($conf): "
	case $ret in
		0) str="$str OK";;
		*) str="$str FAIL";;
	esac
	echo "$str"
	verdict="$verdict
$str"
}

buildme() {
	make clean-all

	./autogen.sh && configure "$conf" || die "configure not working with: $@"
	$make clean
	$make $opts $target
	check_result "$?"
	echo "VERDICT: $verdict"
}

build_make_targets() {
	# defaults
	target=
	buildme
	# defaults, static
	target=static
	buildme
	# defaults, busybox
	target='btrfs.box btrfs.box.static'
	buildme
	# defaults, library
	target="library-test"
	buildme
	# defaults, static library
	target="library-test.static"
	buildme
}

# main()
if ! [ -f configure.ac ]; then
	echo "Please run me from the top directory"
	exit 1
fi

if [ "$1" = "--ccache" ]; then
	shift
	ccache=true
	export CCACHE_DIR=`pwd`/.ccache
	mkdir -p -- "$CCACHE_DIR"
	PATH="/usr/lib64/ccache:$PATH"
	echo "Enable ccache at CCACHE_DIR=$CCACHE_DIR"
	ccache -s
fi

make=make
jobs=16
opts="-j${jobs} $@"
verdict=
target=

conf=
build_make_targets

conf='--disable-documentation'
build_make_targets

conf='--disable-backtrace'
build_make_targets

conf='--disable-convert'
build_make_targets

conf='--disable-zoned'
build_make_targets

conf='--disable-libudev'
build_make_targets

conf='--disable-python'
build_make_targets

conf='--with-convert=ext2'
build_make_targets

conf='--enable-zstd'
build_make_targets

conf='--with-crypto=libgcrypt'
build_make_targets

conf='--with-crypto=libsodium'
build_make_targets

conf='--with-crypto=libkcapi'
build_make_targets

# debugging builds, just the default targets
target='D=1'
buildme

target='D=asan'
buildme

target='D=tsan'
buildme

target='D=ubsan'
buildme

echo "---------------------------------------------------"
echo "$verdict"
