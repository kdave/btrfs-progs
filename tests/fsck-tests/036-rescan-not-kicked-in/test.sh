#!/bin/bash
# Under certain power loss case, btrfs quota tree can be initialized but
# rescan not kicked in. Can be also reproduced by fstests/btrfs/166 but with
# low probability.
#
# This test case verifies a special case when 'btrfs check' does not report
# qgroup accounting difference as an error, thus no false alert for btrfs/166.

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_check "$TOP/btrfs" check "$1"
}

check_all_images
