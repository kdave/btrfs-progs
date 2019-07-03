#!/bin/bash

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs
check_prereq btrfstune
check_prereq btrfs-image

setup_root_helper
prepare_test_dev

if ! check_min_kernel_version 5.0; then
	_not_run "kernel too old, METADATA_UUID support needed"
fi

read_fsid() {
	local dev="$1"

	echo $(run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal \
		dump-super "$dev" | awk '/fsid/ {print $2}' | head -n 1)
}

read_metadata_uuid() {
	local dev="$1"

	echo $(run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal \
		dump-super "$dev" | awk '/metadata_uuid/ {print $2}')
}

check_btrfstune() {
	local fsid

	_log "Checking btrfstune logic"
	# test with random uuid
	run_check $SUDO_HELPER "$TOP/btrfstune" -m "$TEST_DEV"

	# check that specific uuid can set
	run_check $SUDO_HELPER "$TOP/btrfstune" -M d88c8333-a652-4476-b225-2e9284eb59f1 "$TEST_DEV"

	# test that having seed on already changed device doesn't work
	run_mustfail "Managed to set seed on metadata uuid fs" \
		$SUDO_HELPER "$TOP/btrfstune" -S 1 "$TEST_DEV"

	# test that setting both seed and -m|M is forbidden
	run_check_mkfs_test_dev
	run_mustfail "Succeeded setting seed and changing fs uuid" \
		$SUDO_HELPER "$TOP/btrfstune" -S 1 -m "$TEST_DEV"

	# test that having -m|-M on seed device is forbidden
	run_check_mkfs_test_dev
	run_check $SUDO_HELPER "$TOP/btrfstune" -S 1 "$TEST_DEV"
	run_mustfail "Succeded changing fsid on a seed device" \
		$SUDO_HELPER "$TOP/btrfstune" -m "$TEST_DEV"

	# test that using -U|-u on an fs with METADATA_UUID flag is forbidden
	run_check_mkfs_test_dev
	run_check $SUDO_HELPER "$TOP/btrfstune" -m "$TEST_DEV"
	run_mustfail "Succeeded triggering FSID rewrite while METADATA_UUID is active" \
		$SUDO_HELPER "$TOP/btrfstune" -u  "$TEST_DEV"
}

check_dump_super_output() {
	local fsid
	local metadata_uuid
	local dev_item_match
	local old_metadata_uuid

	_log "Checking dump-super output"
	# assert that metadata/fsid match on non-changed fs
	fsid=$(read_fsid "$TEST_DEV")
	metadata_uuid=$(read_metadata_uuid "$TEST_DEV")
	[ "$fsid" = "$metadata_uuid" ] || _fail "fsid ("$fsid") doesn't match metadata_uuid ("$metadata_uuid")"

	dev_item_match=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super \
		"$TEST_DEV" | awk '/dev_item.fsid/ {print $3}')

	[ $dev_item_match = "[match]" ] || _fail "dev_item.fsid doesn't match on non-metadata uuid fs"


	_log "Checking output after fsid change"
	# change metadatauuid and ensure everything in the output is still correct
	old_metadata_uuid=$metadata_uuid
	run_check $SUDO_HELPER "$TOP/btrfstune" -M d88c8333-a652-4476-b225-2e9284eb59f1 "$TEST_DEV"
	fsid=$(read_fsid "$TEST_DEV")
	metadata_uuid=$(read_metadata_uuid "$TEST_DEV")
	dev_item_match=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" \
		inspect-internal dump-super "$TEST_DEV" | awk '/dev_item.fsid/ {print $3}')

	[ "$dev_item_match" = "[match]" ] || _fail "dev_item.fsid doesn't match on metadata_uuid fs"
	[ "$fsid" = "d88c8333-a652-4476-b225-2e9284eb59f1" ] || _fail "btrfstune metadata_uuid change failed"
	[ "$old_metadata_uuid" = "$metadata_uuid" ] || _fail "metadata_uuid changed unexpectedly"

	_log "Checking for incompat textual representation"
	# check for textual output of the new incompat feature
	run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super \
		"$TEST_DEV" | grep -q METADATA_UUID
	[ $? -eq 0 ] || _fail "Didn't find textual representation of METADATA_UUID feature"

	_log "Checking setting fsid back to original"
	# ensure that  setting the fsid back to the original works
	run_check $SUDO_HELPER "$TOP/btrfstune" -M "$old_metadata_uuid" "$TEST_DEV"

	fsid=$(read_fsid "$TEST_DEV")
	metadata_uuid=$(read_metadata_uuid "$TEST_DEV")

	[ "$fsid" = "$metadata_uuid" ] || _fail "fsid and metadata_uuid don't match"
	run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super \
		"$TEST_DEV" | grep -q METADATA_UUID
	[ $? -eq 1 ] || _fail "METADATA_UUID feature still shown as enabled"
}

check_image_restore() {
	local metadata_uuid
	local fsid
	local fsid_restored
	local metadata_uuid_restored

	_log "Testing btrfs-image restore"
	run_check_mkfs_test_dev
	run_check $SUDO_HELPER "$TOP/btrfstune" -m "$TEST_DEV"
	fsid=$(read_fsid "$TEST_DEV")
	metadata_uuid=$(read_metadata_uuid "$TEST_DEV")
	run_mayfail $SUDO_HELPER "$TOP/btrfs-image" "$TEST_DEV" /tmp/test-img.dump
	# erase the fs by creating a new one, wipefs is not sufficient as it just
	# deletes the fs magic string
	run_check_mkfs_test_dev
	run_check $SUDO_HELPER "$TOP/btrfs-image" -r /tmp/test-img.dump "$TEST_DEV"
	fsid_restored=$(read_fsid "$TEST_DEV")
	metadata_uuid_restored=$(read_metadata_uuid "$TEST_DEV")

	[ "$fsid" = "$fsid_restored" ] || _fail "fsid don't match after restore"
	[ "$metadata_uuid" = "$metadata_uuid_restored" ] || _fail "metadata_uuids don't match after restore"
}

check_inprogress_flag() {
	# check the flag is indeed cleared
	run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super \
		"$1" | grep -q 0x1000000001
	[ $? -eq 1 ] || _fail "Found BTRFS_SUPER_FLAG_CHANGING_FSID_V2 set for $1"

	run_check_stdout $SUDO_HELPER $TOP/btrfs inspect-internal dump-super \
		"$2" | grep -q 0x1000000001
	[ $? -eq 1 ] || _fail "Found BTRFS_SUPER_FLAG_CHANGING_FSID_V2 set for $2"
}

check_completed() {
	# check that metadata uuid is indeed completed
	run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super \
		"$1" | grep -q METADATA_UUID
	[ $? -eq 0 ] || _fail "metadata_uuid not set on $1"

	run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-super \
		"$2" | grep -q METADATA_UUID
	[ $? -eq 0 ] || _fail "metadata_uuid not set on $2"
}

check_multi_fsid_change() {
	check_inprogress_flag "$1" "$2"
	check_completed "$1" "$2"
}

failure_recovery() {
	local image1
	local image2
	local loop1
	local loop2
	local devcount

	image1=$(extract_image "$1")
	image2=$(extract_image "$2")
	loop1=$(run_check_stdout $SUDO_HELPER losetup --find --show "$image1")
	loop2=$(run_check_stdout $SUDO_HELPER losetup --find --show "$image2")

	# Mount and unmount, on trans commit all disks should be consistent
	run_check $SUDO_HELPER mount "$loop1" "$TEST_MNT"
	run_check $SUDO_HELPER umount "$TEST_MNT"

	# perform any specific check
	"$3" "$loop1" "$loop2"

	# cleanup
	run_check $SUDO_HELPER losetup -d "$loop1"
	run_check $SUDO_HELPER losetup -d "$loop2"
	rm -f -- "$image1" "$image2"
}

reload_btrfs() {
	run_check $SUDO_HELPER rmmod btrfs
	run_check $SUDO_HELPER modprobe btrfs
}

# for full coverage we need btrfs to actually be a module
modinfo btrfs > /dev/null 2>&1 || _not_run "btrfs must be a module"
run_mayfail $SUDO_HELPER modprobe -r btrfs || _not_run "btrfs must be unloadable"
run_mayfail $SUDO_HELPER modprobe btrfs || _not_run "loading btrfs module failed"

run_check_mkfs_test_dev
check_btrfstune

run_check_mkfs_test_dev
check_dump_super_output

run_check_mkfs_test_dev
check_image_restore

# disk1 is an image which has no metadata uuid flags set and disk2 is part of
# the same fs but has the in-progress flag set. Test that whicever is scanned
# first will result in consistent filesystem.
failure_recovery "./disk1.raw.xz" "./disk2.raw.xz" check_inprogress_flag
reload_btrfs
failure_recovery "./disk2.raw.xz" "./disk1.raw.xz" check_inprogress_flag

reload_btrfs

# disk4 contains an image in with the in-progress flag set and disk 3 is part
# of the same filesystem but has both METADATA_UUID incompat and a new
# metadata uuid set. So disk 3 must always take precedence
failure_recovery "./disk3.raw.xz" "./disk4.raw.xz" check_completed
reload_btrfs
failure_recovery "./disk4.raw.xz" "./disk3.raw.xz" check_completed

# disk5 contains an image which has undergone a successful fsid change more
# than once, disk6 on the other hand is member of the same filesystem but
# hasn't completed its last change. Thus it has both the FSID_CHANGING flag set
# and METADATA_UUID flag set.
failure_recovery "./disk5.raw.xz" "./disk6.raw.xz" check_multi_fsid_change
reload_btrfs
failure_recovery "./disk6.raw.xz" "./disk5.raw.xz" check_multi_fsid_change
