#!/bin/bash
# In order to confirm that btrfsck supports to check a variety of refs, add the
# following cases:
#
# * keyed_block_ref
# * keyed_data_ref
# * shared_block_ref
# * shared_data_ref
# * no_inline_ref (a extent item without inline ref)
# * no_skinny_ref
#
# Special check for lowmem regression
# * block_group_item_false_alert
#   Containing a block group and its first extent at
#   the beginning of leaf.
#   Which caused false alert for lowmem mode.

source $TOP/tests/common

check_prereq btrfs

for img in *.img *.raw.xz
do
	image=$(extract_image $img)

	# Since the return value bug is already fixed, we don't need
	# the old grep hack to detect bug.
	run_check $TOP/btrfs check "$image"
	rm -f "$image"
done
