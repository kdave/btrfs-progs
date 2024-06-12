#!/bin/bash
# Verify mkfs for all currently supported profiles of zoned + raid-stripe-tree

source "$TEST_TOP/common" || exit

check_regular_build
setup_root_helper
setup_nullbdevs 4 128 4
prepare_nullbdevs
TEST_DEV=${nullb_devs[1]}

profiles="dup raid1 raid1c3 raid1c4 raid10"

# The existing supported profiles.
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -O zoned -d single -m single "${nullb_devs[@]}"
run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -O zoned -d single -m DUP "${nullb_devs[@]}"

# RST feature is rejected
run_mustfail "RST feature created" \
	$SUDO_HELPER "$TOP/mkfs.btrfs" -f -O zoned,raid-stripe-tree -d single -m DUP "${nullb_devs[@]}"

for dprofile in $profiles; do
	# Make sure additional data profiles won't enable RST for non-experimental build
	run_mustfail "unsupported profile created" \
		$SUDO_HELPER "$TOP/mkfs.btrfs" -f -O zoned -d "$dprofile" -m DUP "${nullb_devs[@]}"
done

# The old unsupported profiles should fail no matter if experimental build is enabled or not.
run_mustfail "unsupported profile raid56 created" \
	$SUDO_HELPER "$TOP/mkfs.btrfs" -f -O zoned -d raid5 -m raid5 "${nullb_devs[@]}"
run_mustfail "unsupported profile raid56 created" \
	$SUDO_HELPER "$TOP/mkfs.btrfs" -f -O zoned -d raid6 -m raid6 "${nullb_devs[@]}"

cleanup_nullbdevs
