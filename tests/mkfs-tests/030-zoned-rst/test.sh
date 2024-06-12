#!/bin/bash
# Verify mkfs for all currently supported profiles of zoned + raid-stripe-tree

source "$TEST_TOP/common" || exit

check_experimental_build
setup_root_helper
setup_nullbdevs 4 128 4
prepare_nullbdevs
TEST_DEV=${nullb_devs[1]}

profiles="single dup raid1 raid1c3 raid1c4 raid10"

for dprofile in $profiles; do
	for mprofile in $profiles; do
		# It's sufficient to specify only 'zoned', the rst will be enabled
		run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -O zoned -d "$dprofile" -m "$mprofile" "${nullb_devs[@]}"
	done
done

run_mustfail "unsupported profile raid56 created" \
	$SUDO_HELPER "$TOP/mkfs.btrfs" -f -O zoned -d raid5 -m raid5 "${nullb_devs[@]}"
run_mustfail "unsupported profile raid56 created" \
	$SUDO_HELPER "$TOP/mkfs.btrfs" -f -O zoned -d raid6 -m raid6 "${nullb_devs[@]}"

cleanup_nullbdevs
