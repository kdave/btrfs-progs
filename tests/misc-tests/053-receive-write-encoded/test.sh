#!/bin/bash
#
# test that we can send and receive encoded writes for three modes of
# transparent compression: zlib, lzo, and zstd.

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev

here=`pwd`

# assumes the filesystem exists, and does mount, write, snapshot, send, unmount
# for the specified encoding option
send_one() {
	local str
	local subv
	local snap

	algorithm="$1"
	shift
	file="$1"
	shift

	subv="subv-$algorithm"
	snap="snap-$algorithm"

	run_check_mount_test_dev "-o" "compress-force=$algorithm"
	cd "$TEST_MNT" || _fail "cannot chdir to TEST_MNT"

	trucate -s0 "$file"
	chmod a+w "$file"

	run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$subv"
	run_check $SUDO_HELPER dd if=/dev/zero of="$subv/file1" bs=1M count=1
	run_check $SUDO_HELPER dd if=/dev/zero of="$subv/file2" bs=500K count=1
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$subv" "$snap"
	run_check $SUDO_HELPER "$TOP/btrfs" send -f "$file" "$snap" "$@"

	cd "$here" || _fail "cannot chdir back to test directory"
	run_check_umount_test_dev
}

receive_one() {
	local str

	str="$1"
	shift

	run_check_mkfs_test_dev
	run_check_mount_test_dev
	run_check $SUDO_HELPER "$TOP/btrfs" receive "$@" -v -f "$str" "$TEST_MNT"
	run_check_umount_test_dev
	run_check rm -f -- "$str"
}

test_one_write_encoded() {
	local str
	local algorithm

	algorithm="$1"
	shift
	str="$here/stream-$algorithm.stream"

	run_check_mkfs_test_dev
	send_one "$algorithm" "$str" --compressed-data
	receive_one "$str" "$@"
}

test_one_stream_v1() {
	local str
	local algorithm

	algorithm="$1"
	shift
	str="$here/stream-$algorithm.stream"

	run_check_mkfs_test_dev
	send_one "$algorithm" "$str" --proto 1
	receive_one "$str" "$@"
}

test_mix_write_encoded() {
	local strzlib
	local strlzo
	local strzstd

	strzlib="$here/stream-zlib.stream"
	strlzo="$here/stream-lzo.stream"
	strzstd="$here/stream-zstd.stream"

	run_check_mkfs_test_dev

	send_one "zlib" "$strzlib" --compressed-data
	send_one "lzo" "$strlzo" --compressed-data
	send_one "zstd" "$strzstd" --compressed-data

	receive_one "$strzlib"
	receive_one "$strlzo"
	receive_one "$strzstd"
}

test_one_write_encoded "zlib"
test_one_write_encoded "lzo"
test_one_write_encoded "zstd"

# with decompression forced
test_one_write_encoded "zlib" "--force-decompress"
test_one_write_encoded "lzo" "--force-decompress"
test_one_write_encoded "zstd" "--force-decompress"

# send stream v1
test_one_stream_v1 "zlib"
test_one_stream_v1 "lzo"
test_one_stream_v1 "zstd"

# files use a mix of compression algorithms
test_mix_write_encoded
