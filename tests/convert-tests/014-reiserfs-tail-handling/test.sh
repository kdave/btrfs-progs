#!/bin/bash
# Check the various combinations of real blocks, holes, and tails
# Since it's possible to have a valid extent layout that check will
# happily accept AND have garbage in the output, compare the results
# as well.
# We use separate inputs for tails and real blocks so we can determine
# if there was a failure in copying either.

source "$TEST_TOP/common"
source "$TEST_TOP/common.convert"

if ! check_kernel_support_reiserfs >/dev/null; then
	_not_run "no reiserfs support"
fi

setup_root_helper
prepare_test_dev
check_prereq btrfs-convert
check_global_prereq md5sum
check_global_prereq mkreiserfs

printf "%0.sa" {1..8192} > input
printf "%0.sb" {1..8192} > input2

default_mkfs="mkreiserfs -b 4096"
convert_test_preamble '' 'tail conversion test' 16k "$default_mkfs"
convert_test_prep_fs reiserfs $default_mkfs

# Hole alone
run_check $SUDO_HELPER truncate -s 81920 "$TEST_MNT/hole"

# Tail alone
run_check $SUDO_HELPER dd if=input of="$TEST_MNT/1k" bs=1k count=1 \
                      > /dev/null 2>&1

# Single indirect block
run_check $SUDO_HELPER dd if=input of="$TEST_MNT/4k" bs=1k count=4 \
                      > /dev/null 2>&1

# Indirect block + tail
run_check $SUDO_HELPER dd if=input of="$TEST_MNT/5k" bs=1k count=4 \
                      > /dev/null 2>&1
run_check $SUDO_HELPER dd if=input2 of="$TEST_MNT/5k" bs=1k count=1 \
                         seek=4 > /dev/null 2>&1

# Hole followed by tail
run_check $SUDO_HELPER dd if=input of="$TEST_MNT/hole-1k" bs=1k count=1 \
                         seek=4 > /dev/null 2>&1

# Indirect block followed by hole
run_check $SUDO_HELPER dd if=input of="$TEST_MNT/4k-hole" bs=1k count=4 \
                      > /dev/null 2>&1
run_check $SUDO_HELPER truncate -s 81920 "$TEST_MNT/4k-hole"

# Indirect block followed by hole followed by tail
run_check $SUDO_HELPER dd if=input of="$TEST_MNT/4k-hole-1k" bs=1k count=4 \
                      > /dev/null 2>&1
run_check $SUDO_HELPER truncate -s 8192 "$TEST_MNT/4k-hole-1k"
run_check $SUDO_HELPER dd if=input2 of="$TEST_MNT/4k-hole-1k" bs=1k count=1 \
                         seek=8 > /dev/null 2>&1

rm -f input input2

declare -A SUMS
for file in "$TEST_MNT"/*; do
       SUM=$(md5sum "$file")
       SUMS["$file"]=$SUM
done

run_check_umount_test_dev
convert_test_do_convert

run_check_mount_test_dev
for file in "${!SUMS[@]}"; do
       SUM=$(md5sum "$file")
       run_check test "$SUM" = "${SUMS[$file]}"
done
run_check_umount_test_dev
