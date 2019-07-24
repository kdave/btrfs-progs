#!/bin/bash

source "$TEST_TOP/common"

check_prereq btrfs

_log "extracting image default_case.tar.xz"
tar --no-same-owner -xJf default_case.tar.xz || \
	_fail "failed to extract default_case.tar.xz"
check_image test.img

_log "extracting image skinny_case.tar.xz"
tar --no-same-owner -xJf skinny_case.tar.xz || \
	_fail "failed to extract skinny_case.tar.xz"
check_image test.img

rm test.img
