#!/bin/bash
# Test if "mkfs.btrfs --rootdir" would properly copy all the attributes of the
# source directory.

source "$TEST_TOP/common" || exit

setup_root_helper

# Here we need two devices, one as a temporaray btrfs, storing the source
# directory. As we want to setup xattr, which is not supported by tmpfs
# (most modern distros use tmpfs for /tmp).
# So we put the source directory on a fs that reliably supports xattr.
#
# Then the second fs is the real one we mkfs on.
setup_loopdevs 2
prepare_loopdevs

tmp_dev=${loopdevs[1]}
real_dev=${loopdevs[2]}

check_global_prereq setfattr
check_global_prereq getfattr

# Here we don't want to use /tmp, as it's pretty common /tmp is tmpfs, which
# doesn't support xattr.
# Instead we go $TEST_TOP/btrfs-progs-mkfs-tests-027.XXXXXX/ instead.
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f "$tmp_dev"
run_check $SUDO_HELPER mount -t btrfs "$tmp_dev" "$TEST_MNT"

run_check $SUDO_HELPER mkdir "$TEST_MNT/source_dir/"
run_check $SUDO_HELPER chmod 750 "$TEST_MNT/source_dir/"
run_check $SUDO_HELPER chown 1000:1000 "$TEST_MNT/source_dir/"
run_check $SUDO_HELPER setfattr -n user.rootdir "$TEST_MNT/source_dir/"

old_mode=$(run_check_stdout $SUDO_HELPER stat "$TEST_MNT/source_dir/" | grep "Uid:")
run_check $SUDO_HELPER touch "$TEST_MNT/source_dir/foobar"
run_check $SUDO_HELPER setfattr -n user.foobar "$TEST_MNT/source_dir/foobar"

run_check $SUDO_HELPER "$TOP/mkfs.btrfs" --rootdir "$TEST_MNT/source_dir" -f "$real_dev"
run_check $SUDO_HELPER umount "$TEST_MNT"

run_check $SUDO_HELPER mount -t btrfs "$real_dev" "$TEST_MNT"

new_mode=$(run_check_stdout $SUDO_HELPER stat "$TEST_MNT/" | grep "Uid:")
new_rootdir_attr=$(run_check_stdout $SUDO_HELPER getfattr -n user.rootdir --absolute-names "$src_dir")
new_foobar_attr=$(run_check_stdout getfattr -n user.foobar --absolute-names "$src_dir/foobar")

run_check_umount_test_dev "$TEST_MNT"
cleanup_loopdevs

if ! echo "$new_rootdir_attr" | grep -q "user.rootdir" ; then
	_fail "no rootdir xattr found"
fi

if ! echo "$new_foobar_attr"| grep -q "user.foobar" ; then
	_fail "no regular file xattr found"
fi

if [ "$new_mode" != "$old_mode" ]; then
	_fail "mode/uid/gid mismatch"
fi
