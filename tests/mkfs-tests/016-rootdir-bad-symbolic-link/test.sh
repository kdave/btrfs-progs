#!/bin/bash
# Regression test for mkfs.btrfs --rootdir with dangling symlink (points to
# non-existing location)
#
# Since mkfs.btrfs --rootdir will just create symbolic link rather than
# follow it, we shouldn't hit any problem

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
prepare_test_dev

tmp=$(_mktemp_dir mkfs-rootdir)

non_existing="/no/such/file$RANDOM$RANDOM"

if [ -f "$non_existing" ]; then
	_not_run "Some one created $non_existing, which is not expect to exist"
fi

run_check ln -sf "$non_existing" "$tmp/foobar"

run_check "$TOP/mkfs.btrfs" -f --rootdir "$tmp" "$TEST_DEV"
run_check "$TOP/btrfs" check "$TEST_DEV"

rm -rf -- "$tmp"
