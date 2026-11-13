#!/bin/bash
# Verify 'btrfs filesystem mkswapfile' option combinations

source "$TEST_TOP/common" || exit

check_global_prereq file

test_mkswapfile() {
	run_check rm -f -- swapfile
	run_check "$TOP/btrfs" filesystem mkswapfile "$@" swapfile
	run_check file swapfile
}

test_mkswapfile_fail() {
	run_check rm -f -- swapfile
	run_mustfail "invalid parameters accepted" \
		"$TOP/btrfs" filesystem mkswapfile "$@" swapfile
}

# Defaults on running system
test_mkswapfile
test_mkswapfile --size 1G
test_mkswapfile --size 2G
test_mkswapfile --size 1G --pagesize 4K
test_mkswapfile --size 1G --pagesize 16K
test_mkswapfile --size 1G --pagesize 64K
test_mkswapfile --size 2G --pagesize 4K
test_mkswapfile --size 2G --pagesize 16K
test_mkswapfile --size 2G --pagesize 64K

test_mkswapfile_fail --pagesize 1K
test_mkswapfile_fail --pagesize 128K
test_mkswapfile_fail --pagesize 1
test_mkswapfile_fail --pagesize 0

# Failure exit codes were just returning 0 previously. Test failure modes here.
# 40K passes the minimum size check but is too short for any page size
test_mkswapfile_fail --size 40K
# Page count does not fit the 32bit swap header field, fails before allocation
test_mkswapfile_fail --size 32T --pagesize 4K

rm -f -- swapfile
