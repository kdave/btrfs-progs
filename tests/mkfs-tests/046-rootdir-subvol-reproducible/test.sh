#!/bin/bash
# Verify that mkfs.btrfs --rootdir --subvol produces reproducible images.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_global_prereq cmp

tmp=$(_mktemp_dir mkfs-rootdir-subvol-repro)
run_check mkdir -p "$tmp/source/subv/nested" "$tmp/source/dir"
run_check touch "$tmp/source/file" "$tmp/source/subv/foo" "$tmp/source/dir/bar"

image1=$(_mktemp mkfs-rootdir-subvol-repro-1)
image2=$(_mktemp mkfs-rootdir-subvol-repro-2)
fs_uuid=12345678-1234-1234-1234-123456789abc

for img in "$image1" "$image2"; do
	run_check truncate -s 0 "$img"
	run_check truncate -s 256M "$img"
	run_check env SOURCE_DATE_EPOCH=1700000000 DETERMINISTIC_SEED=1 \
		"$TOP/mkfs.btrfs" -f -U "$fs_uuid" --rootdir "$tmp/source" \
		--subvol rw:subv --subvol default-ro:dir "$img"
done
run_check cmp "$image1" "$image2"

rm -rf -- "$tmp"
rm -f -- "$image1" "$image2"
