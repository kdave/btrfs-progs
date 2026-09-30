#!/bin/bash
#
# The stream reader must produce the same tree however the bytes arrive:
# from a file, through a pipe in 7-byte pieces (every header and payload
# spans several reads), and through a pipe in pieces larger than its buffer.
# A truncated stream must fail rather than pass as a shorter tree. The tree
# has payloads on both sides of the reader's buffer size: small inline files
# and multi-extent files with incompressible data sent with --compressed-data.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs
check_prereq fssum
check_global_prereq dd
check_global_prereq head

setup_root_helper
prepare_test_dev

FSSUM_PROG="$INTERNAL_BIN/fssum"
here=$(pwd)
src="$TEST_MNT/src"

run_check_mkfs_test_dev
run_check_mount_test_dev
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$src"
run_check $SUDO_HELPER mkdir -p "$src/dir/sub"
run_check $SUDO_HELPER dd if=/dev/zero of="$src/dir/small" bs=1K count=3 status=none
run_check $SUDO_HELPER dd if=/dev/zero of="$src/dir/sub/medium" bs=64K count=3 status=none
run_check $SUDO_HELPER dd if=/dev/zero of="$src/dir/sub/large" bs=1M count=3 status=none
run_check $SUDO_HELPER dd if=/dev/urandom of="$src/dir/random" bs=256K count=2 status=none
run_check $SUDO_HELPER touch "$src/dir/empty"
run_check $SUDO_HELPER ln -s sub/medium "$src/dir/link"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$src" "$TEST_MNT/snap"
run_check $FSSUM_PROG -A -f -w "$here/snap.fssum" "$TEST_MNT/snap"

_mktemp_local full.stream
run_check $SUDO_HELPER "$TOP/btrfs" send -q --compressed-data -f "$here/full.stream" "$TEST_MNT/snap"

receive_and_verify() {
	local dest="$TEST_MNT/$1"

	run_check $SUDO_HELPER mkdir "$dest"
	shift
	run_check $SUDO_HELPER bash -c "$* | '$TOP/btrfs' receive -q '$dest'"
	run_check $FSSUM_PROG -r "$here/snap.fssum" "$dest/snap"
}

receive_and_verify file "cat '$here/full.stream'"
receive_and_verify tiny-pieces "dd if='$here/full.stream' bs=7 status=none"
receive_and_verify big-pieces "dd if='$here/full.stream' bs=1M status=none"

_mktemp_local trunc.stream
run_check bash -c "head -c \$(( \$(stat -c %s '$here/full.stream') * 2 / 3 )) '$here/full.stream' > '$here/trunc.stream'"
run_check $SUDO_HELPER mkdir "$TEST_MNT/truncated"
run_mustfail "truncated stream accepted" \
	$SUDO_HELPER "$TOP/btrfs" receive -q -f "$here/trunc.stream" "$TEST_MNT/truncated"

run_check_umount_test_dev
rm -f -- "$here/snap.fssum" "$here/full.stream" "$here/trunc.stream"
