#!/bin/bash
# verify that mkfs fills the uuid and otime for FS_TREE

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

# item 3 key (FS_TREE ROOT_ITEM 0) itemoff 14949 itemsize 439
#         generation 4 root_dirid 256 bytenr 30408704 level 0 refs 1
#         lastsnap 0 byte_limit 0 bytes_used 16384 flags 0x0(none)
#         uuid 322826f3-817a-4111-89ff-5481bfd516e2
#         ctime 1521656113.0 (2018-03-21 19:15:13)
#         otime 1521656113.0 (2018-03-21 19:15:13)
#         drop key (0 UNKNOWN.0 0) level 0

run_check_mkfs_test_dev
# match not-all-zeros in the first part
uuid=$(run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-tree -t root "$TEST_DEV" | \
	grep -A 5 "FS_TREE ROOT_ITEM 0" | grep 'uuid ')

if [ $? != 0 ]; then
	_fail "uuid for FS_TREE not found"
fi

if [ "$uuid" = '00000000-0000-0000-0000-000000000000' ]; then
	_fail "uuid for FS_TREE is null"
fi

run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal dump-tree -t root "$TEST_DEV" | \
	grep -A 10 "FS_TREE ROOT_ITEM 0" | grep -q 'otime ' || \
	_fail "otime for FS_TREE not found"
