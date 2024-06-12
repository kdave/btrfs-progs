#!/bin/bash

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

if ! check_kernel_support_reiserfs >/dev/null; then
	_not_run "no reiserfs support"
fi

check_prereq btrfs-convert
check_global_prereq mkreiserfs

setup_root_helper
prepare_test_dev

# Iterate over defaults and options that are not tied to hardware capabilities
# or number of devices
for feature in '' 'block-group-tree'; do
	convert_test reiserfs "$feature" "reiserfs 4k nodesize" 4096 mkreiserfs -b 4096
	convert_test reiserfs "$feature" "reiserfs 16k nodesize" 16384 mkreiserfs -b 4096
	convert_test reiserfs "$feature" "reiserfs 64k nodesize" 65536 mkreiserfs -b 4096
done
