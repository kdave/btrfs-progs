#!/bin/bash
# Verify the csum conversion can still resume after an interruption

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

check_experimental_build
check_injection
setup_root_helper
prepare_test_dev

test_resume_data_csum_generation()
{
	local new_csum="$1"
	local tmp=$(_mktemp "csum-convert")

	# Error at the end of the data csum generation.
	export INJECT="0x4de02239"
	run_mustfail_stdout "error injection not working" \
		"$TOP/btrfstune" --csum "$new_csum" "$TEST_DEV" &> $tmp
	cat "$tmp" >> "$RESULTS"
	if ! grep -q "$INJECT" "$tmp"; then
		rm -f -- "$tmp"
		_fail "csum conversion failed to unexpected reason"
	fi
	rm -f -- "$tmp"
	unset INJECT
	run_check "$TOP/btrfstune" --csum "$new_csum" "$TEST_DEV"
	run_check "$TOP/btrfs" check --check-data-csum "$TEST_DEV"
}

check_injection

run_check_mkfs_test_dev --csum crc32c

# We only mount the filesystem once to populate its contents, later one we
# would never mount the fs (to reduce the dependency on kernel features).
run_check_mount_test_dev
populate_fs
run_check_umount_test_dev

test_resume_data_csum_generation xxhash
test_resume_data_csum_generation blake2
test_resume_data_csum_generation sha256
test_resume_data_csum_generation crc32c
