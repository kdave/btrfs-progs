#!/bin/bash
# Test how btrfs subvolume list prints paths, including all of the weird
# accidental behavior.

source "$TEST_TOP/common" || exit

check_prereq btrfs
check_prereq mkfs.btrfs

setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

cd "$TEST_MNT"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "a"
run_check $SUDO_HELPER mkdir "a/b"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "a/b/c"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "a/b/c/d"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "a/e"

subvol_list_paths() {
	run_check_stdout $SUDO_HELPER "$TOP/btrfs" subvolume list "$@" | sed 's/.*path //'
}

expect_subvol_list_paths() {
	diff -u - <(subvol_list_paths "$@") || _fail "wrong output from btrfs subvolume list $*"
}

### No options ###

# Paths are relative to the given subvolume if they are beneath it and relative
# to the root of the filesystem otherwise.
expect_subvol_list_paths . << EOF
a
a/b/c
a/b/c/d
a/e
EOF

expect_subvol_list_paths a << EOF
a
b/c
b/c/d
e
EOF

expect_subvol_list_paths a/b/c << EOF
a
a/b/c
d
a/e
EOF

# If passed a directory, it's treated as the containing subvolume.
expect_subvol_list_paths a/b << EOF
a
b/c
b/c/d
e
EOF

### -a ###

# Paths are relative to the root of the filesystem. Subvolumes that are not an
# immediate child of the passed subvolume are prefixed with <FS_TREE>/.
expect_subvol_list_paths -a . << EOF
a
<FS_TREE>/a/b/c
<FS_TREE>/a/b/c/d
<FS_TREE>/a/e
EOF

expect_subvol_list_paths -a a << EOF
<FS_TREE>/a
a/b/c
<FS_TREE>/a/b/c/d
a/e
EOF

# If passed a directory, it's treated as the containing subvolume.
expect_subvol_list_paths -a a/b << EOF
<FS_TREE>/a
a/b/c
<FS_TREE>/a/b/c/d
a/e
EOF

### -o ###

# Only immediate children of the passed subvolume are printed, and they are
# printed relative to the root of the filesystem.
expect_subvol_list_paths -o . << EOF
a
EOF

expect_subvol_list_paths -o a << EOF
a/b/c
a/e
EOF

# If passed a directory, it's treated as the containing subvolume.
expect_subvol_list_paths -o a/b << EOF
a/b/c
a/e
EOF

expect_subvol_list_paths -o a/b/c << EOF
a/b/c/d
EOF

expect_subvol_list_paths -o a/b/c/d << EOF
EOF

cd ..
run_check_umount_test_dev
