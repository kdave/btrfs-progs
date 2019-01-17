#!/bin/bash
# test all command line options of btrfstune

source "$TEST_TOP/common"

check_prereq btrfstune

setup_root_helper
prepare_test_dev

test_do_mkfs() {
	run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f "$@" "$TEST_DEV"
}

run_mayfail "$TOP/btrfstune" || true
run_check "$TOP/btrfstune" --help

run_mustfail "must not work on non-existent device" \
	"$TOP/btrfstune" -r file-does-not-exist

test_do_mkfs -O ^extref
run_check "$TOP/btrfstune" -r "$TEST_DEV"

test_do_mkfs -O ^skinny-metadata
run_check "$TOP/btrfstune" -x "$TEST_DEV"

test_do_mkfs -O ^no-holes
run_check "$TOP/btrfstune" -n "$TEST_DEV"

test_do_mkfs
run_check "$TOP/btrfstune" -S 1 "$TEST_DEV"
echo n | run_mayfail "$TOP/btrfstune" -S 0 "$TEST_DEV" || true
run_check "$TOP/btrfstune" -f -S 0 "$TEST_DEV"

run_mustfail "negative number for seeding status" \
	"$TOP/btrfstune" -f -S -1 "$TEST_DEV"

test_do_mkfs
echo n | run_mayfail "$TOP/btrfstune" -u "$TEST_DEV" || true
run_check "$TOP/btrfstune" -f -u "$TEST_DEV"

uuid="e315420e-3a1f-4d81-849e-93b68b85b76f"
baduuid="1-2-3-4-5"
test_do_mkfs
echo n | run_mayfail "$TOP/btrfstune" -U "$uuid" "$TEST_DEV" || true
run_check "$TOP/btrfstune" -f -U "$uuid" "$TEST_DEV"

run_mustfail "non-conforming uuid accepted" \
	"$TOP/btrfstune" -U "$baduuid" "$TEST_DEV"

test_do_mkfs
echo n | run_mayfail "$TOP/btrfstune" -m "$TEST_DEV" || true
run_check "$TOP/btrfstune" -f -m "$TEST_DEV"

uuid="2a9716ee-2786-4baa-ab85-f82c50fa883c"
test_do_mkfs
run_mayfail "$TOP/btrfstune" -M "$uuid" "$TEST_DEV" || true
test_do_mkfs
run_check "$TOP/btrfstune" -f -M "$uuid" "$TEST_DEV"

run_mustfail "non-conforming uuid accepted" \
	"$TOP/btrfstune" -M "$baduuid" "$TEST_DEV"
