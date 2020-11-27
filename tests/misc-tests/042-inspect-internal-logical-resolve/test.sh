#!/bin/bash
# Check if logical-resolve is resolving the paths correctly for different
# subvolume tree scenarios. This used to fail when a child subvolume was
# mounted without the parent subvolume being accessible.

source "$TEST_TOP/common"

setup_root_helper
prepare_test_dev

check_prereq btrfs
check_prereq mkfs.btrfs

check_logical_offset_filename()
{
	local offset

	offset="$1"

	while read file; do
		if [[ "$file" = *"inode "* ]]; then
			_log "$file"
		elif [ ! -f "$file" ]; then
			_fail "path '$file' file cannot be accessed"
		else
			_log "$file"
		fi
	done < <(run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal \
					logical-resolve "$offset" "$TEST_MNT")
}

run_check_mkfs_test_dev
run_check_mount_test_dev

# Create top subvolume called '@'
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/@"

# Create a file in each subvolume of @, and each file will have 2 EXTENT_DATA
# items, and also create a snapshot to have an extent being referenced by two
# different fs trees
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/@/vol1"
vol1id=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal rootid "$TEST_MNT/@/vol1")
run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=150 of="$TEST_MNT/@/vol1/file1"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot "$TEST_MNT/@/vol1" "$TEST_MNT/@/snap1"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/@/vol1/subvol1"
subvol1id=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal rootid "$TEST_MNT/@/vol1/subvol1")
run_check $SUDO_HELPER dd if=/dev/zero bs=1M count=150 of="$TEST_MNT/@/vol1/subvol1/file2"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot "$TEST_MNT/@/vol1/subvol1" \
							"$TEST_MNT/@/vol1/snapshot1"

run_check "$TOP/btrfs" filesystem sync "$TEST_MNT"

run_check_umount_test_dev

run_check $SUDO_HELPER mount -o subvol=/@/vol1 "$TEST_DEV" "$TEST_MNT"
# Create a bind mount to vol1. logical-resolve should avoid bind mounts,
# otherwise the test will fail
run_check $SUDO_HELPER mkdir -p "$TEST_MNT/dir"
run_check mkdir -p mnt2
run_check $SUDO_HELPER mount --bind "$TEST_MNT/dir" mnt2
# Create another bind mount to confuse logical-resolve even more.
# logical-resolve can return the original mount or mnt3, both are valid
run_check mkdir -p mnt3
run_check $SUDO_HELPER mount --bind "$TEST_MNT" mnt3

for offset in $("$TOP/btrfs" inspect-internal dump-tree -t "$vol1id" "$TEST_DEV" |
		awk '/disk byte/ { print $5 }'); do
	check_logical_offset_filename "$offset"
done

run_check_umount_test_dev mnt3
run_check rmdir -- mnt3
run_check_umount_test_dev mnt2
run_check rmdir -- mnt2
run_check_umount_test_dev

run_check $SUDO_HELPER mount -o subvol="/@/vol1/subvol1" "$TEST_DEV" "$TEST_MNT"
for offset in $("$TOP/btrfs" inspect-internal dump-tree -t "$subvol1id" "$TEST_DEV" |
		awk '/disk byte/ { print $5 }'); do
	check_logical_offset_filename "$offset"
done

run_check_umount_test_dev
