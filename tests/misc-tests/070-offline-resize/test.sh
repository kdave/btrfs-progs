#!/bin/bash
#
# Test btrfs filesystem resize --offline functionality
# Tests various resize operations on offline filesystems including:
# - Incremental resize (+1G, 1:+1G)
# - Absolute resize (2G)
# - Invalid operations (multi-device, invalid syntax, shrinking)
#

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

_get_filesystem_size()
{
	local mount_point="$1"

	size=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" filesystem usage -m "$mount_point" | \
		grep "Device size:" | awk '{print $3}' | sed 's/\..*//')
	echo "$size"
}

_create_backing_file()
{
	local path="$1"
	local size_mb="$2"

	run_check truncate -s "${size_mb}M" "$path"
}

# Test offline resize with incremental size (+1G)
_log "Testing offline resize +1G"
backing_file="$(_mktemp backing)"
_create_backing_file "$backing_file" 1024

run_check "$TOP/mkfs.btrfs" -f "$backing_file"
run_check "$TOP/btrfs" filesystem resize --offline "+1G" "$backing_file"

TEST_DEV="$backing_file"
run_check_mount_test_dev
actual_size=$(_get_filesystem_size "$TEST_MNT")
if [ "$actual_size" != "2048" ]; then
	_fail "Size mismatch: expected 2048MiB, got ${actual_size}MiB"
fi
run_check_umount_test_dev
rm -f "$backing_file"

# Test offline resize with device-specific incremental size (1:+1G)
_log "Testing offline resize 1:+1G"
backing_file="$(_mktemp backing)"
_create_backing_file "$backing_file" 1024

run_check "$TOP/mkfs.btrfs" -f "$backing_file"
run_check "$TOP/btrfs" filesystem resize --offline "1:+1G" "$backing_file"

TEST_DEV="$backing_file"
run_check_mount_test_dev
actual_size=$(_get_filesystem_size "$TEST_MNT")
if [ "$actual_size" != "2048" ]; then
	_fail "Size mismatch: expected 2048MiB, got ${actual_size}MiB"
fi
run_check_umount_test_dev
rm -f "$backing_file"

# Test offline resize with absolute size (2G)
_log "Testing offline resize 2G"
backing_file="$(_mktemp backing)"
_create_backing_file "$backing_file" 1024

run_check "$TOP/mkfs.btrfs" -f "$backing_file"
run_check "$TOP/btrfs" filesystem resize --offline "2G" "$backing_file"

TEST_DEV="$backing_file"
run_check_mount_test_dev
actual_size=$(_get_filesystem_size "$TEST_MNT")
if [ "$actual_size" != "2048" ]; then
	_fail "Size mismatch: expected 2048MiB, got ${actual_size}MiB"
fi
run_check_umount_test_dev
rm -f "$backing_file"

# Test offline resize with invalid device id (2:+1G)
_log "Testing offline resize with invalid device id 2:+1G"
backing_file="$(_mktemp backing)"
_create_backing_file "$backing_file" 1024

run_check "$TOP/mkfs.btrfs" -f "$backing_file"
run_mustfail "offline resize should fail with invalid device id" \
	"$TOP/btrfs" filesystem resize --offline "2:+1G" "$backing_file"
rm -f "$backing_file"

# Test offline resize with shrinking (not supported)
_log "Testing offline resize with shrinking -10M"
backing_file="$(_mktemp backing)"
_create_backing_file "$backing_file" 1024

run_check "$TOP/mkfs.btrfs" -f "$backing_file"
run_mustfail "offline resize should not support shrinking" \
	"$TOP/btrfs" filesystem resize --offline "-10M" "$backing_file"
rm -f "$backing_file"

# Test offline resize with cancel (not supported)
_log "Testing offline resize with cancel"
backing_file="$(_mktemp backing)"
_create_backing_file "$backing_file" 1024

run_check "$TOP/mkfs.btrfs" -f "$backing_file"
run_mustfail "offline resize should not support cancel" \
	"$TOP/btrfs" filesystem resize --offline "cancel" "$backing_file"
rm -f "$backing_file"

# Test offline resize with invalid size format
_log "Testing offline resize with invalid size format 1:+1a"
backing_file="$(_mktemp backing)"
_create_backing_file "$backing_file" 1024

run_check "$TOP/mkfs.btrfs" -f "$backing_file"
run_mustfail "offline resize should fail with invalid size format" \
	"$TOP/btrfs" filesystem resize --offline "1:+1a" "$backing_file"
rm -f "$backing_file"

# Test offline resize on multi-device filesystem (should fail)
_log "Testing offline resize on multi-device filesystem"
backing_file1="$(_mktemp backing1)"
backing_file2="$(_mktemp backing2)"

_create_backing_file "$backing_file1" 1024
_create_backing_file "$backing_file2" 1024

run_check "$TOP/mkfs.btrfs" -f "$backing_file1" "$backing_file2"
run_mustfail "offline resize should fail on multi-device filesystem" \
	"$TOP/btrfs" filesystem resize --offline "+1G" "$backing_file1"

rm -f "$backing_file1" "$backing_file2"

# Test that --offline and --enqueue are incompatible
_log "Testing that --offline and --enqueue are incompatible"
backing_file="$(_mktemp backing)"
_create_backing_file "$backing_file" 1024

run_check "$TOP/mkfs.btrfs" -f "$backing_file"
run_mustfail "--offline and --enqueue should be incompatible" \
	"$TOP/btrfs" filesystem resize --offline --enqueue "+1G" "$backing_file"

rm -f "$backing_file"

# Test offline resize on mounted filesystem (should fail)
_log "Testing offline resize on mounted filesystem"
setup_loopdevs 1
prepare_loopdevs

dev="${loopdevs[1]}"
run_check "$TOP/mkfs.btrfs" -f "$dev"

TEST_DEV="$dev"
run_check_mount_test_dev

# Get the backing file path from the loop device
backing_file=$(losetup -l "$dev" | tail -n1 | awk '{print $6}')
run_mustfail "offline resize should fail on mounted filesystem" \
	"$TOP/btrfs" filesystem resize --offline "+1G" "$backing_file"

run_check_umount_test_dev
cleanup_loopdevs
