#!/bin/bash
#
# Verify the batched extent reference repair on multiple devices with
# RAID56 data and RAID1 metadata profiles.

source "$TEST_TOP/common" || exit

check_prereq btrfs-corrupt-block
check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
setup_loopdevs 3
prepare_loopdevs
dev1=${loopdevs[1]}

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -m raid1 -d raid5 "${loopdevs[@]}"
run_check $SUDO_HELPER mount "$dev1" "$TEST_MNT"

# Shared data extents: plain files, reflink copies and a snapshot
generate_dataset small
run_check $SUDO_HELPER cp --reflink=always \
	"$TEST_MNT/small/small.1" "$TEST_MNT/reflink1"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot "$TEST_MNT" "$TEST_MNT/snap"

hashes_before=$(_mktemp batch-raid56)
run_check_stdout $SUDO_HELPER find "$TEST_MNT" -type f -exec sha256sum {} + |
	sort > "$hashes_before"
run_check $SUDO_HELPER umount "$TEST_MNT"

# Corrupt the extent tree root so the extent tree has to be rebuilt
extent_root_bytenr=$($SUDO_HELPER "$TOP/btrfs" inspect-internal dump-tree -r \
	"$dev1" | grep extent | awk '{print $7}')
if [ -z "$extent_root_bytenr" ]; then
	_fail "fail to get extent root bytenr"
fi
run_check $SUDO_HELPER "$INTERNAL_BIN/btrfs-corrupt-block" \
	-l "$extent_root_bytenr" -b 4096 "$dev1"

$SUDO_HELPER "$TOP/btrfs" check "$dev1" >& /dev/null && \
	_fail "btrfs check should detect failure"

run_check $SUDO_HELPER "$TOP/btrfs" check --repair --force \
	--init-extent-tree --batch-size 8 "$dev1"
run_check $SUDO_HELPER "$TOP/btrfs" check "$dev1"

run_check $SUDO_HELPER mount "$dev1" "$TEST_MNT"
hashes_after=$(_mktemp batch-raid56)
run_check_stdout $SUDO_HELPER find "$TEST_MNT" -type f -exec sha256sum {} + |
	sort > "$hashes_after"
run_check $SUDO_HELPER umount "$TEST_MNT"

if ! diff "$hashes_before" "$hashes_after" > /dev/null; then
	_fail "file contents changed by the batched extent tree rebuild"
fi
rm -f -- "$hashes_before" "$hashes_after"

cleanup_loopdevs
