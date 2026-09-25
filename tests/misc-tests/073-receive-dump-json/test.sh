#!/bin/bash
# Verify the json output of "btrfs receive --dump --format json", all the
# stream operations must be available and the values must match the text
# output, the text output itself must stay unchanged

source "$TEST_TOP/common" || exit

check_global_prereq jq
setup_root_helper
prepare_test_dev

run_check_mkfs_test_dev
run_check_mount_test_dev

SUBVOL="$TEST_MNT/subv1"
SNAP1="$TEST_MNT/snap1"
SNAP2="$TEST_MNT/snap2"
EMPTY="$TEST_MNT/empty"
EMPTY_SNAP="$TEST_MNT/empty-snap"

_mktemp_local stream-full
_mktemp_local stream-nodata
_mktemp_local stream-incr
_mktemp_local stream-empty
_mktemp_local dump-text
_mktemp_local dump-json
_mktemp_local dump-json-nodata
_mktemp_local dump-json-incr
_mktemp_local dump-json-empty
_mktemp_local dump-json-noops

# Check that the operation is present in the json dump
json_check_op()
{
	local op="$1"
	local file="$2"

	jq -e --arg op "$op" 'any(.operations[]; .op == $op)' "$file" > /dev/null ||
		_fail "json dump: operation $op not found in $file"
}

# A filename with characters that need escaping in json, the text and json
# outputs must use their own escaping
weird_name="weird"$'\n'"name"$'\t'"with\"quote\"and\\backslash"

# A subvolume with a representative set of operations
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$SUBVOL"
run_check $SUDO_HELPER mkdir "$SUBVOL/dir"
run_check $SUDO_HELPER mkdir "$SUBVOL/dir/sub"
run_check $SUDO_HELPER dd if=/dev/zero of="$SUBVOL/dir/sub/data.bin" bs=4k count=1 status=none
run_check $SUDO_HELPER dd if=/dev/zero of="$SUBVOL/file.bin" bs=4k count=4 status=none
run_check $SUDO_HELPER dd if=/dev/zero of="$SUBVOL/$weird_name" bs=4k count=1 status=none
run_check $SUDO_HELPER truncate -s 8192 "$SUBVOL/truncated.bin"
run_check $SUDO_HELPER cp --reflink=always "$SUBVOL/file.bin" "$SUBVOL/reflink.bin"
run_check $SUDO_HELPER ln -s file.bin "$SUBVOL/symlink"
run_check $SUDO_HELPER ln "$SUBVOL/file.bin" "$SUBVOL/hardlink"
run_check $SUDO_HELPER mkfifo "$SUBVOL/fifo"
run_check $SUDO_HELPER mknod "$SUBVOL/chardev" c 1 3
run_check $SUDO_HELPER chmod 0600 "$SUBVOL/file.bin"
run_check $SUDO_HELPER chown 1234:1234 "$SUBVOL/file.bin"
run_check $SUDO_HELPER setfattr -n user.test -v 'value with "quote" and \backslash' "$SUBVOL/file.bin"

# A minimal stream, just the subvolume without any content
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$EMPTY"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$EMPTY" "$EMPTY_SNAP"
run_check $SUDO_HELPER "$TOP/btrfs" send -f stream-empty "$EMPTY_SNAP"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$SUBVOL" "$SNAP1"
run_check $SUDO_HELPER "$TOP/btrfs" send -f stream-full "$SNAP1"
# Without the file data the extents are reported as update_extent
run_check $SUDO_HELPER "$TOP/btrfs" send --no-data -f stream-nodata "$SNAP1"

# Changes for the incremental stream: rename, delete, write and metadata
run_check $SUDO_HELPER mv "$SUBVOL/file.bin" "$SUBVOL/renamed.bin"
run_check $SUDO_HELPER rm -rf "$SUBVOL/dir/sub"
run_check $SUDO_HELPER dd if=/dev/zero of="$SUBVOL/renamed.bin" bs=4k count=1 seek=8 conv=notrunc status=none
run_check $SUDO_HELPER mkdir "$SUBVOL/newdir"
run_check $SUDO_HELPER dd if=/dev/zero of="$SUBVOL/newdir/newfile.bin" bs=4k count=1 status=none
run_check $SUDO_HELPER setfattr -n user.added -v added "$SUBVOL/renamed.bin"
run_check $SUDO_HELPER setfattr -x user.test "$SUBVOL/renamed.bin"
run_check $SUDO_HELPER chmod 0640 "$SUBVOL/renamed.bin"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot -r "$SUBVOL" "$SNAP2"
run_check $SUDO_HELPER "$TOP/btrfs" send -p "$SNAP1" -f stream-incr "$SNAP2"

# The text output must be unchanged: one operation per line, special
# characters escaped in the C way
run_check_stdout "$TOP/btrfs" receive --dump -f stream-full > dump-text
for op in subvol mkfile mkdir rename symlink link mkfifo mknod write	\
	truncate clone set_xattr chmod chown utimes; do
	grep -q "^$op " dump-text || _fail "text dump: operation $op not found"
done
grep -q '{' dump-text && _fail "text dump: unexpected json output"
grep -q '^name' dump-text && _fail "text dump: newlines not escaped"

# Json output, all the operations and values from the text output must be
# present, the C escaping must not be used
run_check_stdout "$TOP/btrfs" receive --dump --format json -f stream-full > dump-json
for op in subvol mkfile mkdir rename symlink link mkfifo mknod write	\
	truncate clone set_xattr chmod chown utimes; do
	json_check_op "$op" dump-json
done

jq -e '[.operations[] | select(.op == "write" and (.path | endswith("file.bin"))) | .length] | add == 16384' dump-json > /dev/null ||
	_fail "json dump: wrong write length"
jq -e 'any(.operations[]; .op == "clone" and .length == 16384 and (.from | endswith("file.bin")))' dump-json > /dev/null ||
	_fail "json dump: clone parameters not equal to the text dump"
jq -e 'any(.operations[]; .op == "mknod" and .dev == 259)' dump-json > /dev/null ||
	_fail "json dump: mknod device not decoded"
jq -e 'any(.operations[]; .op == "truncate" and .size == 8192)' dump-json > /dev/null ||
	_fail "json dump: truncate size not equal to the text dump"
jq -e 'any(.operations[]; .op == "chmod" and .mode == 384)' dump-json > /dev/null ||
	_fail "json dump: chmod mode not equal to the text dump"
jq -e 'any(.operations[]; .op == "chown" and .uid == 1234 and .gid == 1234)' dump-json > /dev/null ||
	_fail "json dump: chown uid/gid not equal to the text dump"

# Values that need escaping, the decoded xattr value must be the same string
# that was set
xattr_value=$(jq -r '.operations[] | select(.op == "set_xattr") | .value' dump-json)
[[ "$xattr_value" == 'value with "quote" and \backslash' ]] ||
	_fail "json dump: xattr value is mangled: $xattr_value"

# The path with special characters must be decoded to the same name
weird_path=$(jq -r --arg suffix "/$weird_name" '.operations[] | select(.op == "write" and (.path | endswith($suffix))) | .path' dump-json)
[[ "$weird_path" == "./snap1/$weird_name" ]] ||
	_fail "json dump: path not escaped correctly: $weird_path"

# The same stream without the file data
run_check_stdout "$TOP/btrfs" receive --dump --format json -f stream-nodata > dump-json-nodata
json_check_op update_extent dump-json-nodata
jq -e 'any(.operations[]; .op == "update_extent" and (.path | endswith("file.bin")) and .length == 16384)' dump-json-nodata > /dev/null ||
	_fail "json dump: wrong update_extent parameters"

# Multiple operations in one stream, including create, delete, rename, write
# and metadata updates
run_check_stdout "$TOP/btrfs" receive --dump --format json -f stream-incr > dump-json-incr
for op in snapshot rename unlink rmdir write mkfile mkdir set_xattr	\
	remove_xattr chmod utimes; do
	json_check_op "$op" dump-json-incr
done

# A minimal stream
run_check_stdout "$TOP/btrfs" receive --dump --format json -f stream-empty > dump-json-empty
json_check_op subvol dump-json-empty

# A stream without any operation, the dump must still be a valid json document
# with an empty array of operations. The stream is incomplete so the parsing
# fails, the error messages are not part of the output.
printf 'btrfs-stream\0\1\0\0\0' | run_mayfail_stdout "$TOP/btrfs" --format json receive --dump > dump-json-noops
grep -v '^ERROR' dump-json-noops | jq -e '.operations == []' > /dev/null ||
	_fail "json dump: empty stream output is not valid json"

rm -f -- stream-full stream-nodata stream-incr stream-empty	\
	dump-text dump-json dump-json-nodata dump-json-incr dump-json-empty	\
	dump-json-noops
run_check_umount_test_dev
