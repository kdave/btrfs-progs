#!/bin/bash
# Basic check if mkfs supports the runtime feature free-space-tree

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper

setup_loopdevs 4
prepare_loopdevs
dev1=${loopdevs[1]}
tmp=$(_mktemp fst-bitmap)

test_do_mkfs()
{
	run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f "$@"
	if run_check_stdout "$TOP/btrfs" check "$dev1" | grep -iq warning; then
		_fail "warnings found in check output"
	fi
}

test_do_mkfs -m raid1 -d raid0 ${loopdevs[@]}

run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-tree \
	-t free_space "$dev1" > "$tmp.dump-tree"
cleanup_loopdevs

if grep -q FREE_SPACE_BITMAP "$tmp.dump-tree"; then
	rm -f -- "$tmp*"
	_fail "free space bitmap should not be created for empty fs"
fi
rm -f -- "$tmp*"
