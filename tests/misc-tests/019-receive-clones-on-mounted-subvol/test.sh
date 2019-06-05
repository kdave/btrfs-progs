#! /bin/bash
#
# Test that an incremental send operation works when in both snapshots there are
# two directory inodes that have the same number but different generations and
# have an entry with the same name that corresponds to different inodes in each
# snapshot.

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs
check_prereq fssum

setup_root_helper
prepare_test_dev

FSSUM_PROG="$INTERNAL_BIN/fssum"
srcdir=./send-test-dir
rm -rf "$srcdir"
mkdir -p "$srcdir"
run_check chmod a+rw "$srcdir"

run_check_mkfs_test_dev
run_check_mount_test_dev

BLOCK_SIZE=$(stat -f -c %S "$TEST_MNT")

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/foo"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/bar"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/baz"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/snap"

tr '\000' 'A' < /dev/null |
	run_check $SUDO_HELPER dd of="$TEST_MNT/foo/file_a" bs="$BLOCK_SIZE" count=32
tr '\000' 'B' < /dev/null |
	run_check $SUDO_HELPER dd of="$TEST_MNT/bar/file_b" bs="$BLOCK_SIZE" count=32

run_check $SUDO_HELPER cp --reflink=always "$TEST_MNT/foo/file_a" "$TEST_MNT/baz/file_a"
run_check $SUDO_HELPER cp --reflink=always "$TEST_MNT/bar/file_b" "$TEST_MNT/baz/file_b"

# Filesystem looks like:
#
# .
# |--- foo/
# |       |--- file_a
# |--- bar/
# |       |--- file_b
# |--- baz/
# |       |--- file_a                   (clone of "foo/file_a")
# |       |--- file_b                   (clone of "bar/file_b")
# |--- snap/
#

# create snapshots and send streams

run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$TEST_MNT/foo" "$TEST_MNT/snap/foo.0"
run_check $SUDO_HELPER "$TOP/btrfs" send -f "$srcdir/foo.0.snap" "$TEST_MNT/snap/foo.0"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$TEST_MNT/bar" "$TEST_MNT/snap/bar.0"
run_check $SUDO_HELPER "$TOP/btrfs" send -f "$srcdir/bar.0.snap" "$TEST_MNT/snap/bar.0"

run_check $SUDO_HELPER cp --reflink=always "$TEST_MNT/foo/file_a" "$TEST_MNT/foo/file_a.clone"
run_check $SUDO_HELPER rm -f -- "$TEST_MNT/foo/file_a"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$TEST_MNT/foo" \
	"$TEST_MNT/snap/foo.1"
run_check $SUDO_HELPER "$TOP/btrfs" send -p "$TEST_MNT/snap/foo.0" -f "$srcdir/foo.1.snap" \
	"$TEST_MNT/snap/foo.1"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$TEST_MNT/baz" \
	"$TEST_MNT/snap/baz.0"
run_check $SUDO_HELPER "$TOP/btrfs" send -p "$TEST_MNT/snap/foo.1" \
	-c "$TEST_MNT/snap/bar.0" -f "$srcdir/baz.0.snap" \
	"$TEST_MNT/snap/baz.0"

# Filesystem looks like:
#
# .
# |--- foo/
# |       |--- file_a.clone             (clone of "foo/file_a")
# |--- bar/
# |       |--- file_b
# |--- baz/
# |       |--- file_a                   (clone of "foo/file_a")
# |       |--- file_b                   (clone of "bar/file_b")
# |--- snap/
#          |--- bar.0/                  (snapshot of "bar")
#          |         |--- file_b
#          |--- foo.0/                  (snapshot of "foo")
#          |         |--- file_a
#          |--- foo.1/                  (snapshot of "foo")
#          |         |--- file_a.clone
#          |--- baz.0/                  (snapshot of "baz")
#          |         |--- file_a
#          |         |--- file_b

run_check $FSSUM_PROG -A -f -w "$srcdir/foo.0.fssum" "$TEST_MNT/snap/foo.0"
run_check $FSSUM_PROG -A -f -w "$srcdir/foo.1.fssum" "$TEST_MNT/snap/foo.1"
run_check $FSSUM_PROG -A -f -w "$srcdir/bar.0.fssum" "$TEST_MNT/snap/bar.0"
run_check $FSSUM_PROG -A -f -w "$srcdir/baz.0.fssum" "$TEST_MNT/snap/baz.0"

run_check_umount_test_dev
run_check_mkfs_test_dev
run_check_mount_test_dev
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/dest"
run_check_umount_test_dev
run_check_mount_test_dev -o subvol=/dest

run_check $SUDO_HELPER "$TOP/btrfs" receive -f "$srcdir/foo.0.snap" "$TEST_MNT"
run_check $SUDO_HELPER "$TOP/btrfs" receive -f "$srcdir/bar.0.snap" "$TEST_MNT"

# if "dest/" is not correctly stripped from the beginning of the path to
# "foo.0" in the target fs, we would get an error here because the clone source
# "foo.0/file_a" for "foo.1/file_a.clone" can't be found.
run_check $SUDO_HELPER "$TOP/btrfs" receive -f "$srcdir/foo.1.snap" "$TEST_MNT"

# same here, but with send -c instead of -p
run_check $SUDO_HELPER "$TOP/btrfs" receive -f "$srcdir/baz.0.snap" "$TEST_MNT"

run_check $FSSUM_PROG -r "$srcdir/foo.0.fssum" "$TEST_MNT/foo.0"
run_check $FSSUM_PROG -r "$srcdir/foo.1.fssum" "$TEST_MNT/foo.1"
run_check $FSSUM_PROG -r "$srcdir/bar.0.fssum" "$TEST_MNT/bar.0"
run_check $FSSUM_PROG -r "$srcdir/baz.0.fssum" "$TEST_MNT/baz.0"

run_check_umount_test_dev

rm -rf -- "$srcdir"
