#!/bin/bash

source $TOP/tests/common
source $TOP/tests/common.convert

setup_root_helper
prepare_test_dev 512M
check_prereq btrfs-convert

for feature in '' 'extref' 'skinny-metadata' 'no-holes'; do
	convert_test "$feature" "ext4 4k nodesize" 4096 mke2fs -t ext4 -b 4096
	convert_test "$feature" "ext4 8k nodesize" 8192 mke2fs -t ext4 -b 4096
	convert_test "$feature" "ext4 16k nodesize" 16384 mke2fs -t ext4 -b 4096
	convert_test "$feature" "ext4 32k nodesize" 32768 mke2fs -t ext4 -b 4096
	convert_test "$feature" "ext4 64k nodesize" 65536 mke2fs -t ext4 -b 4096
done
