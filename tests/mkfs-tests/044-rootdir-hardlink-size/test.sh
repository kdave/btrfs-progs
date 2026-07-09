#!/bin/bash
#
# Test that "mkfs.btrfs --rootdir" correctly estimates the device size when the
# source directory contains many hardlinks to the same file.  Without inode
# deduplication in the size-estimation pass, each hardlink was counted
# individually, greatly overestimating the required size and causing the output
# file to grow unnecessarily.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs

# Create the rootdir with one 10 MB file and 100 hardlinks to it.
rootdir="$(_mktemp_dir mkfs-rootdir-hardlink-size)"
run_check fallocate -l 10M "${rootdir}/bigfile"
for i in $(seq 1 100); do
	run_check ln "${rootdir}/bigfile" "${rootdir}/hardlink-$i"
done

# Create a test image sized to 1 GiB.
prepare_test_dev 1G

# First test with --shrink: verify the result is well below 1G.
run_check "$TOP/mkfs.btrfs" --shrink --rootdir "$rootdir" "$TEST_DEV"
run_check "$TOP/btrfs" check "$TEST_DEV"

img_size="$(run_check_stdout stat --format=%s "$TEST_DEV")"
sb_total="$(run_check_stdout "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV" |
	awk '/^[[:space:]]*total_bytes[[:space:]]/{print $2; exit}')"

if [ "$img_size" -ne "$sb_total" ]; then
	_fail "shrink: image size $img_size != total_bytes $sb_total"
fi
if [ "$img_size" -ge $((1 * 1024 * 1024 * 1024)) ]; then
	_fail "shrink: image size $img_size >= 1 GiB, shrink did not work"
fi

# Reuse the device at the shrunk size for the non-shrink test.
# If the size estimation correctly deduplicates hardlinks, the data should fit
# without growing the image.
prepare_test_dev "$img_size"
run_check "$TOP/mkfs.btrfs" --rootdir "$rootdir" "$TEST_DEV"
run_check "$TOP/btrfs" check "$TEST_DEV"

img_size2="$(run_check_stdout stat --format=%s "$TEST_DEV")"
sb_total2="$(run_check_stdout "$TOP/btrfs" inspect-internal dump-super "$TEST_DEV" |
	awk '/^[[:space:]]*total_bytes[[:space:]]/{print $2; exit}')"

if [ "$img_size2" -ne "$sb_total2" ]; then
	_fail "non-shrink: image size $img_size2 != total_bytes $sb_total2"
fi
if [ "$img_size2" -gt "$img_size" ]; then
	_fail "non-shrink: image size $img_size2 grew beyond shrunk size $img_size"
fi

rm -rf -- "$rootdir"
