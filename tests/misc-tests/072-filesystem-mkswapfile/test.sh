#!/bin/bash
# Verify 'btrfs filesystem mkswapfile' option combinations

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs
check_global_prereq file

setup_root_helper
prepare_test_dev

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

# Small sizes only, the default 2G may not fit on the test device
test_mkswapfile --size 128M
test_mkswapfile --size 256M
test_mkswapfile --size 128M --pagesize 4K
test_mkswapfile --size 128M --pagesize 16K
test_mkswapfile --size 128M --pagesize 64K
test_mkswapfile --size 256M --pagesize 4K
test_mkswapfile --size 256M --pagesize 16K
test_mkswapfile --size 256M --pagesize 64K

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
