#!/bin/bash
# Verify 'btrfs filesystem mkswapfile' option combinations

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs
check_global_prereq file

setup_root_helper
# Room for the 2G swapfiles
prepare_test_dev 3G

run_check_mkfs_test_dev
run_check_mount_test_dev

swapfile="$TEST_MNT/swapfile"

test_mkswapfile() {
	run_check $SUDO_HELPER rm -f -- "$swapfile"
	run_check $SUDO_HELPER "$TOP/btrfs" filesystem mkswapfile "$@" "$swapfile"
	run_check $SUDO_HELPER file "$swapfile"
}

test_mkswapfile_fail() {
	run_check $SUDO_HELPER rm -f -- "$swapfile"
	run_mustfail "invalid parameters accepted" \
		$SUDO_HELPER "$TOP/btrfs" filesystem mkswapfile "$@" "$swapfile"
	if $SUDO_HELPER test -e "$swapfile"; then
		_fail "swapfile left behind after a failure"
	fi
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
# Fails after the file has been created
test_mkswapfile_fail --size 40K --pagesize 64K

# An existing file is neither overwritten nor removed
run_check $SUDO_HELPER rm -f -- "$swapfile"
run_check $SUDO_HELPER touch "$swapfile"
run_mustfail "existing file overwritten" \
	$SUDO_HELPER "$TOP/btrfs" filesystem mkswapfile "$swapfile"
run_check $SUDO_HELPER test -f "$swapfile"

run_check $SUDO_HELPER rm -f -- "$swapfile"
run_check_umount_test_dev
