#!/bin/bash
# Change fields in super block and do test run of 'btrfs check'

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

check_prereq btrfs-sb-mod

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev
#populate_fs
generate_dataset "small"
generate_dataset "sparse"
run_check_umount_test_dev

# See btrfs-sb-mod --help
fields=(
bytenr
flags
magic
generation
root
chunk_root
log_root
total_bytes
bytes_used
root_dir_objectid
num_devices
sectorsize
nodesize
stripesize
sys_chunk_array_size
chunk_root_generation
compat_flags
compat_ro_flags
incompat_flags
csum_type
root_level
chunk_root_level
log_root_level
cache_generation
uuid_tree_generation
dev_item.devid
dev_item.total_bytes
dev_item.bytes_used
dev_item.io_align
dev_item.io_width
dev_item.sector_size
dev_item.type
dev_item.generation
dev_item.start_offset
dev_item.dev_group
dev_item.seek_speed
dev_item.bandwidth)

# Attempted changes:
#
# - off by one
# - LSB bit flips
# - 32bit boundary bit flips
# - off by sector
# - off by node
# - endianness swap
for field in "${fields[@]}"; do
	for op in -1 +1 ^2 ^4 ^256 ^2147483648 ^4294967296 ^8589934592 +4096 -4096 +16384 -16384 @; do
		run_check $SUDO_HELPER truncate -s 0 image.test
		run_check $SUDO_HELPER cp --reflink=auto --sparse=auto "$TEST_DEV" image.test
		run_check $SUDO_HELPER "$TOP/btrfs-sb-mod" image.test "$field" "$op"
		run_mayfail $SUDO_HELPER "$TOP/btrfs" check image.test
	done
done

run_check $SUDO_HELPER rm -f -- image.test
