#!/bin/bash
# Verify mkfs for all currently supported profiles of zoned + raid-stripe-tree

source "$TEST_TOP/common" || exit

setup_root_helper
setup_loopdevs 4
prepare_loopdevs
TEST_DEV=${loopdevs[1]}

profiles="single dup raid1 raid1c3 raid1c4 raid10"

for dprofile in $profiles; do
	for mprofile in $profiles; do
		# It's sufficient to specify only 'zoned', the rst will be enabled
		run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -O zoned -d "$dprofile" -m "$mprofile" "${loopdevs[@]}"
	done
done

run_mustfail "unsupported profile raid56 created" \
	$SUDO_HELPER "$TOP/mkfs.btrfs" -f -O zoned -d raid5 -m raid5 "${loopdevs[@]}"
run_mustfail "unsupported profile raid56 created" \
	$SUDO_HELPER "$TOP/mkfs.btrfs" -f -O zoned -d raid6 -m raid6 "${loopdevs[@]}"

cleanup_loopdevs
