#!/bin/bash
# Basic test for mkfs.btrfs --inode-flags --rootdir. Create a dataset and use it as
# rootdir, then various inode-flags and verify the flag is properly set.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs
check_global_prereq lsattr

setup_root_helper
prepare_test_dev

tmp=$(_mktemp_dir mkfs-rootdir)

write_file()
{
	local path="$1"
	local size="$2"

	run_check dd if=/dev/zero of="$path" bs="$size" count=1 status=noxfer > /dev/null 2>&1
}

check_nodatacow()
{
	local path="$1"

	lsattr "$TEST_MNT/$path" | grep -q C || _fail "missing NODATACOW flag for $path"
}

write_file "$tmp/file1" 64K
write_file "$tmp/file2" 64K
run_check mkdir -p "$tmp/subv" "$tmp/nocow_subv" "$tmp/nocow_dir/dir2"
write_file "$tmp/subv/file3" 64K
write_file "$tmp/nocow_subv/file4" 64K
write_file "$tmp/nocow_dir/dir2/file5" 64K
write_file "$tmp/nocow_dir/file6" 64K
write_file "$tmp/nocow_file1" 64K

run_check_mkfs_test_dev --rootdir "$tmp"	\
	--inode-flags "nodatacow:nocow_subv"	\
	--subvol "nocow_subv"			\
	--inode-flags "nodatacow:nocow_dir"	\
	--inode-flags "nodatacow:nocow_file1"

run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"

run_check_mount_test_dev
check_nodatacow "nocow_subv"
check_nodatacow "nocow_subv/file4"
check_nodatacow "nocow_dir"
check_nodatacow "nocow_dir/file6"
check_nodatacow "nocow_dir/dir2/file5"
check_nodatacow "nocow_file1"
run_check lsattr -R "$TEST_MNT"
run_check_umount_test_dev

run_check rm -rf -- "$tmp"
