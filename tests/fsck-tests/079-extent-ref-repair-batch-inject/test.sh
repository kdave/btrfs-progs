#!/bin/bash
#
# Verify that a failure to commit the transaction of an extent reference
# repair batch is propagated as an error, and that the repairs of an
# uncommitted batch are not reported as repaired.
#
# The transaction commit failure is simulated by error injection at the
# commit of the first full batch, thus this requires an error injection
# build (make D=1) and is skipped otherwise.

source "$TEST_TOP/common" || exit

check_prereq btrfs-corrupt-block
check_prereq mkfs.btrfs
check_prereq btrfs

check_injection

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev
generate_dataset small
run_check_umount_test_dev

# Corrupt the extent tree root so the extent tree has to be rebuilt
extent_root_bytenr=$($SUDO_HELPER "$TOP/btrfs" inspect-internal dump-tree -r \
	"$TEST_DEV" | grep extent | awk '{print $7}')
if [ -z "$extent_root_bytenr" ]; then
	_fail "fail to get extent root bytenr"
fi
run_check $SUDO_HELPER "$INTERNAL_BIN/btrfs-corrupt-block" \
	-l "$extent_root_bytenr" -b 4096 "$TEST_DEV"

# Fail the commit of the first full batch
tmp=$(_mktemp batch-inject)
export INJECT="0xf5b47937"
run_mustfail_stdout "batched repair should fail on injected commit error" \
	"$TOP/btrfs" check --repair --force --init-extent-tree --batch-size 8 \
	"$TEST_DEV" &> "$tmp"
cat "$tmp" >> "$RESULTS"
if ! grep -q "$INJECT" "$tmp"; then
	rm -f -- "$tmp"
	_fail "repair failed for an unexpected reason"
fi
if grep -q "Repaired extent references" "$tmp"; then
	rm -f -- "$tmp"
	_fail "repairs of an uncommitted batch were reported as repaired"
fi
rm -f -- "$tmp"
unset INJECT

# Without the injection the rebuild completes and the check passes
run_check $SUDO_HELPER "$TOP/btrfs" check --repair --force \
	--init-extent-tree --batch-size 8 "$TEST_DEV"
run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
