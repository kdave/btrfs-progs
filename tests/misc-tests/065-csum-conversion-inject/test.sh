#!/bin/bash
# Verify the csum conversion can still resume after an interruption

source "$TEST_TOP/common" || exit
source "$TEST_TOP/common.convert" || exit

check_experimental_build
setup_root_helper
prepare_test_dev

check_injection()
{
	local tmp_output=$(mktemp --tmpdir btrfs-progs-check-injection.XXXXXX)
	local ret

	run_check_mkfs_test_dev --csum crc32c

	# This is for the very first transaction commit.
	export INJECT="0x3964edd9"
	"$TOP/btrfstune" --csum xxhash "$TEST_DEV" &> "$tmp_output"
	ret=$?
	cat "$tmp_output" >> "$RESULTS"
	if [ "$ret" -eq 0 ]; then
		_not_run "this test requires debug build with error injection"
	fi
	if ! grep -q "$INJECT" "$tmp_output"; then
		rm "$tmp_output"
		unset INJECT
		_fail "csum conversion failed for unexpected reasons."
	fi
	rm "$tmp_output"
	unset INJECT
}

test_resume_data_csum_generation()
{
	local new_csum="$1"

	# Error at the end of the data csum generation.
	export INJECT="0x4de02239"
	run_mustfail "error injection not working" "$TOP/btrfstune" \
		--csum "$new_csum" "$TEST_DEV"
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
