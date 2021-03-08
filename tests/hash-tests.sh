#!/bin/sh
# Test all supported hash algorithms on all backends on the sample test vectors
# This requires all crypto backends available for full coverage.

make=make
opts="-j16 $@"
verdict=

die() {
	echo "ERROR: $@"
	exit 1
}

buildme() {
	make clean-all

	echo "BUILD WITH: $1"
	./autogen.sh && configure \
		--disable-documentation --disable-convert --disable-python \
		--with-crypto="$1" || die "configure not working with: $@"
	$make clean
	$make $opts hash-vectest
	if ./hash-vectest; then
		verdict="$verdict
$1: OK"
	fi
}

# main()
if ! [ -f configure.ac ]; then
	echo "Please run me from the top directory"
	exit 1
fi

buildme builtin
buildme libgcrypt
buildme libsodium
buildme libkcapi

echo "VERDICT:"
echo "$verdict"
