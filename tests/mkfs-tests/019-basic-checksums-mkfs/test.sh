#!/bin/bash
#
# Test creating images with all supported checksums, mount checks are separate

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

if ! [ -f "/sys/fs/btrfs/features/supported_checksums" ]; then
	_not_run "kernel support for checksums missing"
fi

test_mkfs_checksum()
{
	local csum

	csum="$1"
	run_check_stdout $SUDO_HELPER "$TOP/mkfs.btrfs" -f --csum "$csum" "$TEST_DEV" | grep -q "Checksum:.*$csum"
	run_check $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV"
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
}

for csum in $(cat /sys/fs/btrfs/features/supported_checksums); do
	test_mkfs_checksum "$csum"
done
