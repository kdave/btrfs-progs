#!/bin/bash
# Corrupt a filesystem that is using freespace tree and then ensure that
# btrfs check is able to repair it. This tests correct detection/repair of
# both a FREE_SPACE_EXTENT based FST and a FREE_SPACE_BITMAP based FST.

source "$TEST_TOP/common"

setup_root_helper
prepare_test_dev 256M

check_prereq btrfs
check_prereq mkfs.btrfs
check_global_prereq grep
check_global_prereq tail
check_global_prereq head
check_global_prereq cut

# wrapper for btrfs-corrupt-item
# $1: Type of item we want to corrupt - extent or bitmap
corrupt_fst_item()
{
	local type
	local objectid
	local offset
	type="$1"

	if [[ $type == "bitmap" ]]; then
		type=200
		objectid=$("$TOP/btrfs" inspect-internal dump-tree -t 10 "$TEST_DEV" | \
			grep -o "[[:digit:]]* FREE_SPACE_BITMAP [[:digit:]]*" | \
			cut -d' ' -f1 | tail -2 | head -1)
		offset=$("$TOP/btrfs" inspect-internal dump-tree -t 10 "$TEST_DEV" | \
			grep -o "[[:digit:]]* FREE_SPACE_BITMAP [[:digit:]]*" | \
			cut -d' ' -f3 | tail -2 | head -1)
		_log "Corrupting $objectid,FREE_SPACE_BITMAP,$offset"
	elif [[ $type == "extent" ]]; then
		type=199
		objectid=$("$TOP/btrfs" inspect-internal dump-tree -t 10 "$TEST_DEV" | \
			grep -o "[[:digit:]]* FREE_SPACE_EXTENT [[:digit:]]*" | \
			cut -d' ' -f1 | tail -2 | head -1)
		offset=$("$TOP/btrfs" inspect-internal dump-tree -t 10 "$TEST_DEV" | \
			grep -o "[[:digit:]]* FREE_SPACE_EXTENT [[:digit:]]*" | \
			cut -d' ' -f3 | tail -2 | head -1)
		_log "Corrupting $objectid,FREE_SPACE_EXTENT,$offset"
	else
		_fail "Unknown item type for corruption"
	fi

	run_check "$INTERNAL_BIN/btrfs-corrupt-block" -r 10 -K "$objectid,$type,$offset" \
		-f offset "$TEST_DEV"
}

if ! [ -f "/sys/fs/btrfs/features/free_space_tree" ]; then
	_not_run "kernel does not support free-space-tree feature"
	exit
fi

run_check_mkfs_test_dev -n 4k
run_check_mount_test_dev -oclear_cache,space_cache=v2

# create files which will populate the FST
for i in {0..9}; do
	for j in {1..300}; do
		run_check $SUDO_HELPER fallocate -l 4k "$TEST_MNT/file.$j$i" &
	done
	wait
done

run_check_umount_test_dev

# now corrupt one of the bitmap items
corrupt_fst_item "bitmap"
check_image "$TEST_DEV"

# change the freespace such that we now have at least one free_space_extent
# object
run_check_mount_test_dev
rm -rf "$TEST_MNT/file.*"
run_check $SUDO_HELPER fallocate -l 50m "$TEST_MNT/file"
run_check_umount_test_dev

# now corrupt an extent
corrupt_fst_item "extent"
check_image "$TEST_DEV"
