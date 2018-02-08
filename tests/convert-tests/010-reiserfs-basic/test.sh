#!/bin/bash

source "$TEST_TOP/common"
source "$TEST_TOP/common.convert"

if ! check_kernel_support_reiserfs >/dev/null; then
	_not_run "no reiserfs support"
fi

setup_root_helper
prepare_test_dev
check_prereq btrfs-convert
check_global_prereq mkreiserfs

for feature in '' 'extref' 'skinny-metadata' 'no-holes'; do
	convert_test reiserfs "$feature" "reiserfs 4k nodesize" 4096 mkreiserfs -b 4096
	convert_test reiserfs "$feature" "reiserfs 8k nodesize" 8192 mkreiserfs -b 4096
	convert_test reiserfs "$feature" "reiserfs 16k nodesize" 16384 mkreiserfs -b 4096
	convert_test reiserfs "$feature" "reiserfs 32k nodesize" 32768 mkreiserfs -b 4096
	convert_test reiserfs "$feature" "reiserfs 64k nodesize" 65536 mkreiserfs -b 4096
done
