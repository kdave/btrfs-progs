#!/bin/bash
#
# Receive in reverse direction must not throw an error if it can find an
# earlier "sent" parent.  In general, shows a backup+sync setup between two (or
# more) PCs with an external drive.

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs
check_global_prereq dd

declare -a roots
i_pc1=1
# An external drive used to backup and carry profile.
i_ext=2
i_pc2=3
roots[$i_pc1]="$TEST_MNT/pc1"
roots[$i_ext]="$TEST_MNT/external"
roots[$i_pc2]="$TEST_MNT/pc2"

setup_root_helper
mkdir -p "${roots[@]}"
setup_loopdevs 3
prepare_loopdevs
for i in `seq 3`; do
	TEST_DEV="${loopdevs[$i]}"
	TEST_MNT="${roots[$i]}"
	run_check_mkfs_test_dev
	run_check_mount_test_dev
	run_check $SUDO_HELPER mkdir -p "$TEST_MNT/.snapshots"
done

run_check_update_file()
{
	run_check $SUDO_HELPER cp --reflink "${roots[$1]}/profile/$2" "${roots[$1]}/profile/staging"
	run_check $SUDO_HELPER dd if=/dev/urandom conv=notrunc bs=4K count=4 seek=$3 "of=${roots[$1]}/profile/staging"
	run_check $SUDO_HELPER mv "${roots[$1]}/profile/staging" "${roots[$1]}/profile/$2"
}
run_check_copy_snapshot_with_diff()
{
	_mktemp_local send.data
	run_check $SUDO_HELPER "$TOP/btrfs" send -f send.data -p "${roots[$1]}/.snapshots/$2" "${roots[$1]}/.snapshots/$3"
	run_check $SUDO_HELPER "$TOP/btrfs" receive -f send.data "${roots[$4]}/.snapshots"
}
run_check_backup_profile()
{
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "${roots[$1]}/profile" "${roots[$1]}/.snapshots/$3"
	run_check_copy_snapshot_with_diff "$1" "$2" "$3" "$i_ext"
	# Don't keep old snapshot in pc
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete "${roots[$1]}/.snapshots/$2"
}
run_check_restore_profile()
{
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot "${roots[$1]}/.snapshots/$2" "${roots[$1]}/profile"
}
run_check_copy_fresh_backup_and_replace_profile()
{
    run_check_copy_snapshot_with_diff "$i_ext" "$2" "$3" "$1"
    # IRL, it would be a nice idea to make a backup snapshot before deleting.
    run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete "${roots[$1]}/profile"
    run_check_restore_profile "$1" "$3"
    # Don't keep old snapshot in pc
    run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete "${roots[$1]}/.snapshots/$2"
}


run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "${roots[$i_pc1]}/profile"
run_check $SUDO_HELPER dd if=/dev/urandom bs=4K count=16 "of=${roots[$i_pc1]}/profile/day1"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "${roots[$i_pc1]}/profile" "${roots[$i_pc1]}/.snapshots/day1"
_mktemp_local send.data
run_check $SUDO_HELPER "$TOP/btrfs" send -f send.data "${roots[$i_pc1]}/.snapshots/day1"
run_check $SUDO_HELPER "$TOP/btrfs" receive -f send.data "${roots[$i_ext]}/.snapshots"

run_check_update_file "$i_pc1" day1 2
run_check_backup_profile "$i_pc1" day1 day2

_mktemp_local send.data
run_check $SUDO_HELPER "$TOP/btrfs" send -f send.data "${roots[$i_ext]}/.snapshots/day2"
run_check $SUDO_HELPER "$TOP/btrfs" receive -f send.data "${roots[$i_pc2]}/.snapshots"
run_check_restore_profile "$i_pc2" day2
run_check_update_file "$i_pc2" day1 3
run_check_backup_profile "$i_pc2" day2 day3

run_check_update_file "$i_pc2" day1 4
run_check_backup_profile "$i_pc2" day3 day4

run_check_copy_fresh_backup_and_replace_profile "$i_pc1" day2 day4
run_check_update_file "$i_pc1" day1 5
run_check_backup_profile "$i_pc1" day4 day5

run_check_copy_fresh_backup_and_replace_profile "$i_pc2" day4 day5
run_check_update_file "$i_pc2" day1 6
run_check_backup_profile "$i_pc2" day5 day6

run_check_umount_test_dev "${loopdevs[@]}"
rmdir "${roots[@]}"
rm -f send.data
cleanup_loopdevs
