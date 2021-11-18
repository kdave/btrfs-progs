#!/bin/bash
# Tests 'btrfs fi usage' reports correct space/ratio with various RAID profiles

source "$TEST_TOP/common"

check_prereq btrfs
setup_root_helper
setup_loopdevs 4
prepare_loopdevs

TEST_DEV=${loopdevs[1]}

report_numbers()
{
	local vars

	vars=($(run_check_stdout $SUDO_HELPER "$TOP/btrfs" filesystem usage -b "$TEST_MNT" | awk '
	/Data ratio/ { ratio=$3 }
	a {dev_alloc=$2; exit}
	/Data,(DUP|RAID[0156][C34]{0,2}|single)/ { size=substr($2,6,length($2)-6); a=1 }
END {print ratio " " size " " dev_alloc}'))

	echo "${vars[@]}"
}

test_dup()
{
	local vars
	local data_chunk_size
	local used_on_dev
	local data_ratio

	run_check_mkfs_test_dev -ddup
	run_check_mount_test_dev
	vars=($(report_numbers))
	data_chunk_size=${vars[1]}
	used_on_dev=${vars[2]}
	data_ratio=${vars[0]}

	[[ $used_on_dev -eq $((2*$data_chunk_size)) ]] ||
		_fail "DUP inconsistent chunk/device usage. Chunk: $data_chunk_size Device: $used_on_dev"

	[[ "$data_ratio" = "2.00" ]] ||
		_fail "DUP: Unexpected data ratio: $data_ratio (must be 2)"
	run_check_umount_test_dev
}

test_raid1()
{
	local vars
	local data_chunk_size
	local used_on_dev
	local data_ratio

	# single is not raid1 but it has the same characteristics so put it in here
	# as well
	for i in single,1.00 raid1,2.00 raid1c3,3.00 raid1c4,4.00; do

		# this allows to set the tuples to $1 and $2 respectively
		OLDIFS=$IFS
		IFS=","
		set -- $i
		IFS=$OLDIFS

		run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -d$1 ${loopdevs[@]}
		run_check_mount_test_dev
		vars=($(report_numbers))
		data_chunk_size=${vars[1]}
		used_on_dev=${vars[2]}
		data_ratio=${vars[0]}

		[[ $used_on_dev -eq $data_chunk_size ]] ||
			_fail "$1 inconsistent chunk/device usage. Chunk: $data_chunk_size Device: $used_on_dev"

		[[ "$data_ratio" = "$2" ]] ||
			_fail "$1: Unexpected data ratio: $data_ratio (must be $2)"

		run_check_umount_test_dev
	done
}

test_raid0()
{
	local vars
	local data_chunk_size
	local used_on_dev
	local data_ratio

	run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -draid0 ${loopdevs[@]}
	run_check_mount_test_dev
	vars=($(report_numbers))
	data_chunk_size=${vars[1]}
	used_on_dev=${vars[2]}
	data_ratio=${vars[0]}

	# Divide by 4 since 4 loopp devices are setup
	[[ $used_on_dev -eq $(($data_chunk_size / 4)) ]] ||
		_fail "raid0 inconsistent chunk/device usage. Chunk: $data_chunk_size Device: $used_on_dev"

	[[ $data_ratio = "1.00" ]] ||
		_fail "raid0: Unexpected data ratio: $data_ratio (must be 1.5)"
	run_check_umount_test_dev
}

test_raid56()
{
	local vars
	local data_chunk_size
	local used_on_dev
	local data_ratio

	for i in raid5,1.33,3 raid6,2.00,2; do

		# This allows to set the tuples to $1 and $2 respectively
		OLDIFS=$IFS
		IFS=","
		set -- $i
		IFS=$OLDIFS

		run_check $SUDO_HELPER "$TOP/mkfs.btrfs" -f -d$1 ${loopdevs[@]}
		run_check_mount_test_dev
		vars=($(report_numbers))
		data_chunk_size=${vars[1]}
		used_on_dev=${vars[2]}
		data_ratio=${vars[0]}

		[[ $used_on_dev -eq $(($data_chunk_size / $3)) ]] ||
			_fail "$i inconsistent chunk/device usage. Chunk: $data_chunk_size Device: $used_on_dev"

		[[ $data_ratio = "$2" ]] ||
			_fail "$1: Unexpected data ratio: $data_ratio (must be $2)"

		run_check_umount_test_dev
	done
}

test_dup
test_raid1
test_raid0
test_raid56

cleanup_loopdevs
