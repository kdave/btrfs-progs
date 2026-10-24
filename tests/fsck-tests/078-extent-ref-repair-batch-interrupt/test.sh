#!/bin/bash
#
# Verify that a batched extent reference repair interrupted between
# committed batches (the process gets killed) leaves the filesystem in a
# state where a fresh rebuild completes and the file contents are
# unchanged.

source "$TEST_TOP/common" || exit

check_prereq btrfs-corrupt-block
check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

# Enough data extents to keep the rebuild busy, shared with a snapshot
for i in $(seq 1 100); do
	run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/file.$i" \
		bs=64K count=4 status=noxfer > /dev/null 2>&1
done
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot "$TEST_MNT" "$TEST_MNT/snap"

hashes_before=$(_mktemp batch-interrupt)
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

# Kill the repair once at least one batch has been committed, which is
# reported by a "Repaired extent references" message
repair_out=$(_mktemp batch-interrupt)
$SUDO_HELPER "$TOP/btrfs" check --repair --force --init-extent-tree \
	--batch-size 4 "$TEST_DEV" > "$repair_out" 2>&1 &
repair_pid=$!
for i in $(seq 1 300); do
	grep -q "Repaired extent references" "$repair_out" 2>/dev/null && break
	sleep 0.1
done
kill -9 "$repair_pid" 2> /dev/null
wait "$repair_pid" 2> /dev/null
cat "$repair_out" >> "$RESULTS"

# A fresh rebuild must complete and leave the data unchanged
run_check $SUDO_HELPER "$TOP/btrfs" check --repair --force \
	--init-extent-tree --batch-size 4 "$TEST_DEV"
run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"

run_check_mount_test_dev
hashes_after=$(_mktemp batch-interrupt)
run_check_stdout $SUDO_HELPER find "$TEST_MNT" -type f -exec sha256sum {} + |
	sort > "$hashes_after"
run_check_umount_test_dev

if ! diff "$hashes_before" "$hashes_after" > /dev/null; then
	_fail "file contents changed by the interrupted batched repair"
fi
rm -f -- "$hashes_before" "$hashes_after" "$repair_out"
