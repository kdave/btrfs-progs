#!/bin/bash

source $top/tests/common

echo "extracting image default_case.tar.xz" >> $RESULT
tar xJf default_case.tar.xz || \
	_fail "failed to extract default_case.tar.xz"
check_image test.img

echo "extracting image skinny_case.tar.xz" >> $RESULT
tar xJf skinny_case.tar.xz || \
	_fail "failed to extract skinny_case.tar.xz"
check_image test.img

rm test.img
