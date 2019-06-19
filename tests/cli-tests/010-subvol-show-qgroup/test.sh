#!/bin/bash
# simple test for 'subvol show' output

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

# quotas not enabled, no qgroup associated to subv2
run_check $SUDO_HELPER "$TOP"/btrfs subvolume create "$TEST_MNT"/subv2
run_check $SUDO_HELPER "$TOP"/btrfs subvolume show "$TEST_MNT"/subv2

# enable
run_check $SUDO_HELPER "$TOP"/btrfs quota enable "$TEST_MNT"

# autocreated qgroup
run_check $SUDO_HELPER "$TOP"/btrfs subvolume create "$TEST_MNT"/subv1
rootid=$(run_check_stdout $SUDO_HELPER "$TOP"/btrfs inspect rootid "$TEST_MNT"/subv1)
run_check $SUDO_HELPER "$TOP"/btrfs qgroup limit -e 1G "0/$rootid" "$TEST_MNT"

run_check $SUDO_HELPER "$TOP"/btrfs qgroup show "$TEST_MNT"

# no limits
run_check $SUDO_HELPER "$TOP"/btrfs subvolume show "$TEST_MNT"
# 1G limit for exclusive
run_check $SUDO_HELPER "$TOP"/btrfs subvolume show "$TEST_MNT"/subv1
# no limits
run_check $SUDO_HELPER "$TOP"/btrfs subvolume show "$TEST_MNT"/subv2

run_check_umount_test_dev
