#!/bin/bash
# create a base image, convert to btrfs, remove all files, rollback the reiserfs image

source "$TEST_TOP/common"
source "$TEST_TOP/common.convert"

if ! check_kernel_support_reiserfs >/dev/null; then
	_not_run "no reiserfs support"
fi

setup_root_helper
prepare_test_dev
check_prereq btrfs-convert
check_global_prereq mkreiserfs

# simple wrapper for a convert test
# $1: btrfs features, argument to -O
# $2: message
# $3: nodesize value
# $4 + rest: command to create the reiserfs image
do_test() {
	local features
	local msg
	local nodesize
	local CHECKSUMTMP
	local here

	features="$1"
	msg="$2"
	nodesize="$3"
	shift 3
	convert_test_preamble "$features" "$msg" "$nodesize" "$@"
	convert_test_prep_fs reiserfs "$@"
	populate_fs
	CHECKSUMTMP=$(_mktemp convert)
	convert_test_gen_checksums "$CHECKSUMTMP"

	run_check_umount_test_dev

	convert_test_do_convert "$features" "$nodesize"

	run_check_mount_test_dev
	convert_test_post_check_checksums "$CHECKSUMTMP"

	here=$(pwd)
	cd "$TEST_MNT" || _fail "cannot cd to TEST_MNT"
	# reiserfs_saved/image must not be deleted
	run_mayfail $SUDO_HELPER find "$TEST_MNT"/ -mindepth 1 -path '*reiserfs_saved' -prune -o -exec rm -vrf "{}" \;
	cd "$here"
	run_check "$TOP/btrfs" filesystem sync "$TEST_MNT"
	run_check_umount_test_dev
	convert_test_post_rollback reiserfs

	run_check_mount_convert_dev reiserfs
	convert_test_post_check_checksums "$CHECKSUMTMP"
	run_check_umount_test_dev

	# mount again and verify checksums
	run_check_mount_convert_dev reiserfs
	convert_test_post_check_checksums "$CHECKSUMTMP"
	run_check_umount_test_dev

	rm "$CHECKSUMTMP"
}

for feature in '' 'extref' 'skinny-metadata' 'no-holes'; do
	do_test "$feature" "reiserfs 4k nodesize" 4096 mkreiserfs -b 4096
	do_test "$feature" "reiserfs 8k nodesize" 8192 mkreiserfs -b 4096
	do_test "$feature" "reiserfs 16k nodesize" 16384 mkreiserfs -b 4096
	do_test "$feature" "reiserfs 32k nodesize" 32768 mkreiserfs -b 4096
	do_test "$feature" "reiserfs 64k nodesize" 65536 mkreiserfs -b 4096
done
