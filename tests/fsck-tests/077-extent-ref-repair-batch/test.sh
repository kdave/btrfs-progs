#!/bin/bash
#
# Verify that the batched commit mode of the extent reference repair
# ("btrfs check --repair --init-extent-tree --batch-size") rebuilds the
# data extent references correctly on a filesystem with reflinks, a
# snapshot and hard links, and that invalid option combinations are
# rejected.

source "$TEST_TOP/common" || exit

check_prereq btrfs-corrupt-block
check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

# The option is only valid in the extent tree rebuild mode
run_mustfail "--batch-size is rejected without --init-extent-tree" \
	$SUDO_HELPER "$TOP/btrfs" check --force --repair --batch-size 8 "$TEST_DEV"
run_mustfail "--batch-size 0 is rejected" \
	$SUDO_HELPER "$TOP/btrfs" check --force --repair --init-extent-tree \
		--batch-size 0 "$TEST_DEV"
run_mustfail "--batch-size larger than the maximum is rejected" \
	$SUDO_HELPER "$TOP/btrfs" check --force --repair --init-extent-tree \
		--batch-size 4097 "$TEST_DEV"

run_check_mkfs_test_dev
run_check_mount_test_dev

# Various kinds of shared data extents: plain files, reflink copies, hard
# links and a snapshot sharing all of them
generate_dataset small
run_check $SUDO_HELPER cp --reflink=always \
	"$TEST_MNT/small/small.1" "$TEST_MNT/reflink1"
run_check $SUDO_HELPER cp --reflink=always \
	"$TEST_MNT/small/small.2" "$TEST_MNT/reflink2"
run_check $SUDO_HELPER ln "$TEST_MNT/small/small.3" "$TEST_MNT/hardlink1"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot "$TEST_MNT" "$TEST_MNT/snap"

hashes_before=$(_mktemp batch-hashes)
run_check_stdout $SUDO_HELPER find "$TEST_MNT" -type f -exec sha256sum {} + |
	sort > "$hashes_before"
run_check_umount_test_dev

# Corrupt the extent tree root so the extent tree has to be rebuilt
extent_root_bytenr=$($SUDO_HELPER "$TOP/btrfs" inspect-internal dump-tree -r \
	"$TEST_DEV" | grep extent | awk '{print $7}')
if [ -z "$extent_root_bytenr" ]; then
	_fail "fail to get extent root bytenr"
fi
run_check $SUDO_HELPER "$INTERNAL_BIN/btrfs-corrupt-block" \
	-l "$extent_root_bytenr" -b 4096 "$TEST_DEV"

$SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV" >& /dev/null && \
	_fail "btrfs check should detect failure"

repair_out=$(_mktemp batch-repair)
run_check_stdout $SUDO_HELPER "$TOP/btrfs" check --repair --force \
	--init-extent-tree --batch-size 8 "$TEST_DEV" > "$repair_out"
grep -q "Repaired extent references" "$repair_out" || \
	_fail "no extent references were repaired"

run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"

# The files and their contents must survive the rebuild
run_check_mount_test_dev
hashes_after=$(_mktemp batch-hashes)
run_check_stdout $SUDO_HELPER find "$TEST_MNT" -type f -exec sha256sum {} + |
	sort > "$hashes_after"
run_check_umount_test_dev

if ! diff "$hashes_before" "$hashes_after" > /dev/null; then
	_fail "file contents changed by the batched extent tree rebuild"
fi
rm -f -- "$hashes_before" "$hashes_after" "$repair_out"
