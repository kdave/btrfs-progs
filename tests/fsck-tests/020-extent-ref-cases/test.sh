#!/bin/bash
# In order to confirm that 'btrfs check' supports checking a variety of refs,
# add the following cases:
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
#
# Special cases with some rare backref types
# * reloc tree
#   For both fs tree and data reloc tree.
#   Special for its backref pointing to itself and its short life span.

source "$TEST_TOP/common"

check_prereq btrfs

check_image() {
	run_check "$TOP/btrfs" check "$1"
}

check_all_images
