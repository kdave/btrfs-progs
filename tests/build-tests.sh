#!/bin/sh
# test various compilation options
# - 32bit, 64bit
# - dynamic, static
# - various configure options
#
# Arguments: anything will be passed to 'make', eg. define CC, D, V
#
# Requirements for full coverage:
# - static version of all libs
# - 32bit/64bit libraries, also the static variants

make=make
opts="-j16 $@"

conf=
target=

function die() {
	echo "ERROR: $@"
	exit 1
}

function check_result() {
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

function buildme() {
	make clean-all

	./autogen.sh && configure "$conf" || die "configure not working with: $@"
	$make clean
	$make $opts $target
	check_result "$?"
	echo "VERDICT: $verdict"
}

function build_make_targets() {
	# defaults
	target=
	buildme
	# defaults, static
	target=static
	buildme
	# defaults, 32bit
	target="EXTRA_CFLAGS=-m32"
	buildme
	# defaults, 64bit
	target="EXTRA_CFLAGS=-m64"
	buildme
	# defaults, library
	target="library-test"
	buildme
}

# main()
if ! [ -f configure.ac ]; then
	echo "Please run me from the top directory"
	exit 1
fi

verdict=
conf=
build_make_targets

conf='--disable-documentation'
build_make_targets

conf='--disable-backtrace'
build_make_targets

conf='--disable-convert'
build_make_targets

echo "---------------------------------------------------"
echo "$verdict"
