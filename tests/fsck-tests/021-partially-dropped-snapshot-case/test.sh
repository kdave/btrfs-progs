#!/bin/bash
# confirm whether btrfsck supports to check a partially dropped snapshot

source $TOP/tests/common

check_prereq btrfs

for img in *.img
do
	image=$(extract_image $img)
	run_check_stdout $TOP/btrfs check "$image" 2>&1 |
		grep -q "Errors found in extent allocation tree or chunk allocation"
	if [ $? -eq 0 ]; then
		rm -f "$image"
		_fail "unexpected error occurred when checking $img"
	fi
	rm -f "$image"
done
