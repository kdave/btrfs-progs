#!/bin/bash
# Verify that source atime updates do not change reproducible rootdir images.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_global_prereq cmp
setup_root_helper

setup_loopdevs 1
prepare_loopdevs
source_dev=${loopdevs[1]}

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f "$source_dev"
run_check $SUDO_HELPER mount -t btrfs -o relatime "$source_dev" "$TEST_MNT"
run_check $SUDO_HELPER chmod 0777 "$TEST_MNT"

source_dir="$TEST_MNT/source"
run_check mkdir "$source_dir"
run_check dd if=/dev/zero of="$source_dir/file" bs=4096 count=1 status=none

source_date_epoch=1700000000
old_timestamp=$((source_date_epoch - 86400))
run_check touch -d "@$old_timestamp" "$source_dir/file"

image1=$(_mktemp mkfs-rootdir-atime-1)
image2=$(_mktemp mkfs-rootdir-atime-2)
fs_uuid=12345678-1234-1234-1234-123456789abc

run_check env SOURCE_DATE_EPOCH="$source_date_epoch" DETERMINISTIC_SEED=1 \
	"$TOP/mkfs.btrfs" -f -U "$fs_uuid" --rootdir "$source_dir" "$image1"

source_atime=$(run_check_stdout stat --format=%X "$source_dir/file")
if [[ "$source_atime" -le "$source_date_epoch" ]]; then
	_fail "reading the source did not advance atime on the relatime mount"
fi

run_check env SOURCE_DATE_EPOCH="$source_date_epoch" DETERMINISTIC_SEED=1 \
	"$TOP/mkfs.btrfs" -f -U "$fs_uuid" --rootdir "$source_dir" "$image2"
run_check cmp "$image1" "$image2"

run_check $SUDO_HELPER umount "$TEST_MNT"
run_check $SUDO_HELPER mount -t btrfs -o loop,ro "$image2" "$TEST_MNT"

image_times=$(run_check_stdout $SUDO_HELPER stat --format='%X %Y %Z' \
	"$TEST_MNT/file")
read -r image_atime image_mtime image_ctime <<< "$image_times"
if [[ "$image_atime" -ne "$source_date_epoch" ]]; then
	_fail "image atime $image_atime does not match SOURCE_DATE_EPOCH"
fi
if [[ "$image_ctime" -ne "$source_date_epoch" ]]; then
	_fail "image ctime $image_ctime does not match SOURCE_DATE_EPOCH"
fi
if [[ "$image_mtime" -ne "$old_timestamp" ]]; then
	_fail "image mtime $image_mtime does not preserve the older source mtime"
fi

run_check $SUDO_HELPER umount "$TEST_MNT"
cleanup_loopdevs
rm -f -- "$image1" "$image2"
