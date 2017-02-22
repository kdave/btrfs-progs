#! /bin/bash
#
# Test that an incremental send operation works when in both snapshots there are
# two directory inodes that have the same number but different generations and
# have an entry with the same name that corresponds to different inodes in each
# snapshot.

# temporary, until the test gets adapted
exit 0

seq=`basename $0`
seqres=$RESULT_DIR/$seq
echo "QA output created by $seq"

tmp=/tmp/$$
status=1    # failure is the default!
trap "_cleanup; exit \$status" 0 1 2 3 15

_cleanup()
{
    cd /
    rm -fr $send_files_dir
    rm -f $tmp.*
}

# get standard environment, filters and checks
. ./common/rc
. ./common/filter

# real QA test starts here
_supported_fs btrfs
_supported_os Linux
_require_test
_require_scratch
_require_cloner
_require_fssum

send_files_dir=$TEST_DIR/btrfs-test-$seq

rm -f $seqres.full
rm -fr $send_files_dir
mkdir $send_files_dir

_scratch_mkfs >>$seqres.full 2>&1
_scratch_mount

BLOCK_SIZE=$(_get_block_size $SCRATCH_MNT)

# create source fs

$BTRFS_UTIL_PROG subvolume create $SCRATCH_MNT/foo | _filter_scratch
$BTRFS_UTIL_PROG subvolume create $SCRATCH_MNT/bar | _filter_scratch
$BTRFS_UTIL_PROG subvolume create $SCRATCH_MNT/baz | _filter_scratch
$BTRFS_UTIL_PROG subvolume create $SCRATCH_MNT/snap | _filter_scratch

$XFS_IO_PROG -s -f -c "pwrite -S 0xaa -b $((32 * $BLOCK_SIZE)) 0 $((32 * $BLOCK_SIZE))" \
             $SCRATCH_MNT/foo/file_a | _filter_xfs_io_blocks_modified
$XFS_IO_PROG -s -f -c "pwrite -S 0xbb -b $((32 * $BLOCK_SIZE)) 0 $((32 * $BLOCK_SIZE))" \
             $SCRATCH_MNT/bar/file_b | _filter_xfs_io_blocks_modified

$CLONER_PROG $SCRATCH_MNT/{foo/file_a,baz/file_a}
$CLONER_PROG $SCRATCH_MNT/{bar/file_b,baz/file_b}

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

$BTRFS_UTIL_PROG subvolume snapshot -r $SCRATCH_MNT/foo \
    $SCRATCH_MNT/snap/foo.0 > /dev/null

$BTRFS_UTIL_PROG send $SCRATCH_MNT/snap/foo.0   \
    -f $send_files_dir/foo.0.snap               \
    2>&1 1>/dev/null | _filter_scratch

$BTRFS_UTIL_PROG subvolume snapshot -r          \
    $SCRATCH_MNT/bar $SCRATCH_MNT/snap/bar.0    \
    > /dev/null

$BTRFS_UTIL_PROG send $SCRATCH_MNT/snap/bar.0   \
    -f $send_files_dir/bar.0.snap               \
    2>&1 1>/dev/null | _filter_scratch

$CLONER_PROG $SCRATCH_MNT/foo/file_a{,.clone}
rm $SCRATCH_MNT/foo/file_a

$BTRFS_UTIL_PROG subvolume snapshot -r          \
    $SCRATCH_MNT/foo $SCRATCH_MNT/snap/foo.1    \
    > /dev/null

$BTRFS_UTIL_PROG send                           \
    -p $SCRATCH_MNT/snap/foo.0                  \
    -f $send_files_dir/foo.1.snap               \
    $SCRATCH_MNT/snap/foo.1                     \
    2>&1 1>/dev/null | _filter_scratch

$BTRFS_UTIL_PROG subvolume snapshot -r          \
    $SCRATCH_MNT/baz $SCRATCH_MNT/snap/baz.0    \
    > /dev/null

$BTRFS_UTIL_PROG send                           \
    -p $SCRATCH_MNT/snap/foo.1                  \
    -c $SCRATCH_MNT/snap/bar.0                  \
    -f $send_files_dir/baz.0.snap               \
    $SCRATCH_MNT/snap/baz.0                     \
    2>&1 1>/dev/null | _filter_scratch

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

run_check $FSSUM_PROG -A -f -w $send_files_dir/foo.0.fssum $SCRATCH_MNT/snap/foo.0
run_check $FSSUM_PROG -A -f -w $send_files_dir/foo.1.fssum $SCRATCH_MNT/snap/foo.1
run_check $FSSUM_PROG -A -f -w $send_files_dir/bar.0.fssum $SCRATCH_MNT/snap/bar.0
run_check $FSSUM_PROG -A -f -w $send_files_dir/baz.0.fssum $SCRATCH_MNT/snap/baz.0

_scratch_unmount
_scratch_mkfs >>$seqres.full 2>&1
_scratch_mount
$BTRFS_UTIL_PROG subvolume create $SCRATCH_MNT/dest | _filter_scratch
_scratch_unmount
_scratch_mount -o subvol=/dest

$BTRFS_UTIL_PROG receive            \
    -f $send_files_dir/foo.0.snap   \
    $SCRATCH_MNT                    \
    2>&1 | _filter_scratch
$BTRFS_UTIL_PROG receive            \
    -f $send_files_dir/bar.0.snap   \
    $SCRATCH_MNT                    \
    2>&1 | _filter_scratch

# if "dest/" is not correctly stripped from the beginning of the path to "foo.0" in the target fs,
# we would get an error here because the clone source "foo.0/file_a" for "foo.1/file_a.clone" can't be found.
$BTRFS_UTIL_PROG receive            \
    -f $send_files_dir/foo.1.snap   \
    $SCRATCH_MNT                    \
    2>&1 | _filter_scratch

# same here, but with send -c instead of -p
$BTRFS_UTIL_PROG receive            \
    -f $send_files_dir/baz.0.snap   \
    $SCRATCH_MNT                    \
    2>&1 | _filter_scratch

run_check $FSSUM_PROG -r $send_files_dir/foo.0.fssum $SCRATCH_MNT/foo.0
run_check $FSSUM_PROG -r $send_files_dir/foo.1.fssum $SCRATCH_MNT/foo.1
run_check $FSSUM_PROG -r $send_files_dir/bar.0.fssum $SCRATCH_MNT/bar.0
run_check $FSSUM_PROG -r $send_files_dir/baz.0.fssum $SCRATCH_MNT/baz.0

status=0
exit
