#!/bin/bash
#
# The stream reader must produce the same tree however the bytes arrive:
# from a file, through a pipe in 7-byte pieces (every header and payload
# spans several reads), and through a pipe in pieces larger than its buffer.
# Two streams back to back, in a file or through a pipe, must both be
# received, although the reader has already read into the second one when the
# first ends. With -e a receive must stop right after the END command and
# leave the rest to a second receive -e on the same descriptor. A stream cut
# inside a command header or a payload must fail rather than pass as a shorter
# tree. The tree has payloads on both sides of the reader's buffer size: small
# inline files and multi-extent files with incompressible data sent with
# --compressed-data.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs
check_prereq fssum
check_global_prereq dd
check_global_prereq truncate

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

run_check $SUDO_HELPER dd if=/dev/urandom of="$src/dir/new" bs=4K count=3 status=none
run_check $SUDO_HELPER rm "$src/dir/small"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$src" "$TEST_MNT/snap2"
run_check $FSSUM_PROG -A -f -w "$here/snap2.fssum" "$TEST_MNT/snap2"

_mktemp_local incr.stream
run_check $SUDO_HELPER "$TOP/btrfs" send -q -p "$TEST_MNT/snap" -f "$here/incr.stream" "$TEST_MNT/snap2"

# Receive the stream through a pipe in pieces of the given block size
receive_and_verify() {
	local bs="$1"
	local dest="$TEST_MNT/bs-$bs"

	run_check $SUDO_HELPER mkdir "$dest"
	dd if="$here/full.stream" bs="$bs" status=none |
		$SUDO_HELPER "$TOP/btrfs" receive -q "$dest" ||
		_fail "receive in pieces of $bs bytes failed"
	run_check $FSSUM_PROG -r "$here/snap.fssum" "$dest/snap"
}

receive_and_verify 7
receive_and_verify 1M

run_check $SUDO_HELPER mkdir "$TEST_MNT/file"
run_check $SUDO_HELPER "$TOP/btrfs" receive -q -f "$here/full.stream" "$TEST_MNT/file"
run_check $FSSUM_PROG -r "$here/snap.fssum" "$TEST_MNT/file/snap"

# Both snapshots of the two streams must have arrived under the given path
verify_both() {
	run_check $FSSUM_PROG -r "$here/snap.fssum" "$1/snap"
	run_check $FSSUM_PROG -r "$here/snap2.fssum" "$1/snap2"
}

# From a file, so the buffered read always crosses into the second stream
_mktemp_local concat.stream
cat "$here/full.stream" "$here/incr.stream" > "$here/concat.stream" ||
	_fail "cannot write the concatenated stream"
run_check $SUDO_HELPER mkdir "$TEST_MNT/concat"
run_check $SUDO_HELPER "$TOP/btrfs" receive -q -f "$here/concat.stream" "$TEST_MNT/concat"
verify_both "$TEST_MNT/concat"

# The two streams through a pipe
run_check $SUDO_HELPER mkdir "$TEST_MNT/concat-pipe"
cat "$here/full.stream" "$here/incr.stream" |
	$SUDO_HELPER "$TOP/btrfs" receive -q "$TEST_MNT/concat-pipe" ||
	_fail "receive of two streams from a pipe failed"
verify_both "$TEST_MNT/concat-pipe"

# With -e a receive stops after the END command of the first stream
run_check $SUDO_HELPER mkdir "$TEST_MNT/end"
run_check $SUDO_HELPER "$TOP/btrfs" receive -q -e -f "$here/concat.stream" "$TEST_MNT/end"
run_check $FSSUM_PROG -r "$here/snap.fssum" "$TEST_MNT/end/snap"
if [ -e "$TEST_MNT/end/snap2" ]; then
	_fail "receive -e did not stop at the end of the first stream"
fi

# Two receives with -e, one per stream, read from the same descriptor, which
# the first must leave right after its END command
receive_both_with_end() {
	local dest="$1"

	run_check $SUDO_HELPER mkdir "$dest"
	$SUDO_HELPER "$TOP/btrfs" receive -q -e "$dest" ||
		_fail "first receive -e on a shared descriptor failed"
	$SUDO_HELPER "$TOP/btrfs" receive -q -e "$dest" ||
		_fail "second receive -e on a shared descriptor failed"
	verify_both "$dest"
}

receive_both_with_end "$TEST_MNT/end-file" < "$here/concat.stream"
receive_both_with_end "$TEST_MNT/end-pipe" < <(cat "$here/full.stream" "$here/incr.stream")

# A stream is a 17-byte header and then commands, each a 10-byte header with
# its payload; the last is END, which has no payload. Cut the stream one byte
# into the first payload, and inside the END header
receive_must_fail_truncated() {
	local size="$1"
	local dest="$2"

	run_check cp "$here/full.stream" "$here/trunc.stream"
	run_check truncate -s "$size" "$here/trunc.stream"
	run_check $SUDO_HELPER mkdir "$dest"
	run_mustfail "stream cut to $size bytes accepted" \
		$SUDO_HELPER "$TOP/btrfs" receive -q -f "$here/trunc.stream" "$dest"
}

_mktemp_local trunc.stream
size=$(stat -c %s "$here/full.stream")
receive_must_fail_truncated $(( 17 + 10 + 1 )) "$TEST_MNT/cut-payload"
receive_must_fail_truncated $(( size - 1 )) "$TEST_MNT/cut-end"

run_check_umount_test_dev
rm -f -- "$here/snap.fssum" "$here/snap2.fssum" "$here/full.stream" \
	"$here/incr.stream" "$here/concat.stream" "$here/trunc.stream"
