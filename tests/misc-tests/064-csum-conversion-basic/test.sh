#!/bin/bash
#
# Verify the csum conversion works as expected.
#
source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

check_experimental_build
setup_root_helper
prepare_test_dev

convert_to_csum()
{
	local new_csum=$1

	run_check "$TOP/btrfstune" --csum "$new_csum" "$TEST_DEV"
	run_check "$TOP/btrfs" check --check-data-csum "$TEST_DEV"
}

run_check_mkfs_test_dev --csum crc32c

# We only mount the fs once to populate its contents,
# later one we would never mount the fs (to reduce the dependency on
# kernel features).
run_check_mount_test_dev
populate_fs
run_check_umount_test_dev

convert_to_csum xxhash
convert_to_csum blake2
convert_to_csum sha256
convert_to_csum crc32c
