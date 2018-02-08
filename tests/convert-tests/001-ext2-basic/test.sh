#!/bin/bash

source "$TEST_TOP/common"
source "$TEST_TOP/common.convert"

setup_root_helper
prepare_test_dev
check_prereq btrfs-convert
check_global_prereq mke2fs

for feature in '' 'extref' 'skinny-metadata' 'no-holes'; do
	convert_test ext2 "$feature" "ext2 4k nodesize" 4096 mke2fs -b 4096
	convert_test ext2 "$feature" "ext2 8k nodesize" 8192 mke2fs -b 4096
	convert_test ext2 "$feature" "ext2 16k nodesize" 16384 mke2fs -b 4096
	convert_test ext2 "$feature" "ext2 32k nodesize" 32768 mke2fs -b 4096
	convert_test ext2 "$feature" "ext2 64k nodesize" 65536 mke2fs -b 4096
done
