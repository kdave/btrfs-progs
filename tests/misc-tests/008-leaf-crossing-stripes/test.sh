#!/bin/bash
# test if btrfs-convert creates a filesystem without leaf crossing stripes

source "$TEST_TOP/common"

check_prereq btrfs-convert
check_prereq btrfs

# In my test, it happened in 514M~560M, 737M~769M, 929M~917M,
# and HAVE_ERROR=((size + 1) / 2) % 2 if size >= 970
#
SIZE_FROM=514
SIZE_END=560
A_PRIME_NUM=17
for ((size = SIZE_FROM; size <= SIZE_END; size += A_PRIME_NUM)); do
	run_check truncate -s "$size"M "$IMAGE"
	run_check mkfs.ext4 -F "$IMAGE"
	run_check "$TOP/btrfs-convert" "$IMAGE"
	run_check_stdout "$TOP/btrfs" check "$IMAGE" 2>&1 |
		grep -q "crossing stripe boundary" &&
		_fail "leaf crossing stripes after btrfs-convert"
done

# grep will expectedly fail
exit 0
