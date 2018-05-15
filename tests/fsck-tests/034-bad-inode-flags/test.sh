#!/bin/bash
# In order to confirm that 'btrfs check' supports checking symlinks
# with immutable/append attributes that are not possible to set by standard
# syscall or ioctl so they're handled as corruption

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_mustfail "check should report errors about inode flags" \
        $SUDO_HELPER "$TOP/btrfs" check "$1"
}

check_all_images
