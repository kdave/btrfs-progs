#!/bin/bash
# Ensure that clearning ino cache removes all related items

source "$TEST_TOP/common"

check_prereq btrfs

setup_root_helper

image=$(extract_image "./ino-cache-enabled.raw.xz")

run_check "$TOP/btrfs" check --clear-ino-cache "$image"
run_check "$TOP/btrfs" check "$image"

# Check for FREE_INO items for toplevel subvol
item_count=$(run_check_stdout "$TOP/btrfs" inspect-internal dump-tree -t fs "$image" |
	grep -c 'item [0-9].* key (FREE_INO')
[ $item_count -eq 0 ] || _fail "FREE_INO items for toplevel subvolume present"
# Check for bitmap item for toplevel subvol
item_count=$(run_check_stdout "$TOP/btrfs" inspect-internal dump-tree -t fs "$image" |
	grep -c '(FREE_SPACE')
[ $item_count -eq 0 ] || _fail "FREE_SPACE items for toplevel subvolume present"

# Check for FREE_INO items for subvolume
item_count=$(run_check_stdout "$TOP/btrfs" inspect-internal dump-tree -t 256 "$image" |
	grep -c 'item [0-9].* key (FREE_INO')
[ $item_count -eq 0 ] || _fail "ino cache items for subvolume present"
# Check for bitmap item for subvolume
item_count=$(run_check_stdout "$TOP/btrfs" inspect-internal dump-tree -t 256 "$image" |
	grep -c '(FREE_SPACE')
[ $item_count -eq 0 ] || _fail "FREE_SPACE items for subvolume present"

# Check for FREE_INO items for snapshot
item_count=$(run_check_stdout "$TOP/btrfs" inspect-internal dump-tree -t 257 "$image" |
	grep -c 'item [0-9].* key (FREE_INO')
[ $item_count -eq 0 ] || _fail "ino cache items for snapshot present"
# Check for bitmap item for snapshot
item_count=$(run_check_stdout "$TOP/btrfs" inspect-internal dump-tree -t 257 "$image" |
	grep -c '(FREE_SPACE')
[ $item_count -eq 0 ] || _fail "FREE_SPACE items for snapshot present"

# Finally test that the csum tree is empty as ino cache also uses it. At this
# point all ino items/extents should have been deleted hence the csum tree should
# be empty
item_count=$(run_check_stdout "$TOP/btrfs" inspect-internal dump-tree -t csum "$image" |
	sed  -n -e 's/^.* items \([0-9]*\).*/\1/p')

[ $item_count -eq 0 ] || _fail "csum tree not empty"

rm -f -- "$image"
