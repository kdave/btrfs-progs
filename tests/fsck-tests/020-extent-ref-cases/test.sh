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

source $TOP/tests/common

check_prereq btrfs

for img in *.img
do
	image=$(extract_image $img)
	run_check_stdout $TOP/btrfs check "$image" 2>&1 |
		grep -q "Errors found in extent allocation tree or chunk allocation" &&
		_fail "unexpected error occurred when checking $img"
	rm -f "$image"
done
