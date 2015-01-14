#!/bin/bash

source $top/tests/common

# Check file list for leaf corruption, no regular/preallocated
# file extent case.
# Corrupted leaf is 20832256, which contains inode 1862~1872
#
# 1862, ref from leaf 20828160 key 24(DIR_ITEM)
# 1863, ref from leaf 605388 item key 11(DIR_ITEM)
# 1864, no ref to rebuild, no need to rebuild
# 1865, ref from leaf 19767296 key 23(DIR_ITEM)
# 1866-1868 no ref to rebuild, all refs in corrupted leaf
# 1869, ref from leaf 4976640 key 22(DIR_ITEM)
# 1870 no ref to rebuild, all refs in corrupted leaf
# 1871, ref from leaf 19746816 key 38(DIR_ITEM)
# 1872, ref from leaf 19767296 key 14(DIR_ITEM)
# The list format is:
# INO SIZE MODE NAME
# INO: inode number
# SIZE: file size, only checked for regular file
# MODE: raw file mode, get from stat
# NAME: file name
leaf_no_data_ext_list=(
	1862 0 40700 "install.d"
	1862 0 40700 "install.d"
	1863 0 40700 "gdb"
	1865 0 40700 "iptables"
	1869 0 40700 "snmp"
	1871 0 100700 "machine-id"
	1872 0 100700 "adjtime"
)

generate_leaf_corrupt_no_data_ext()
{
	dest=$1
	echo "generating leaf_corrupt_no_data_ext.btrfs-image" >> $RESULT
	tar xJf ./no_data_extent.tar.xz || \
		_fail "failed to extract leaf_corrupt_no_data_ext.btrfs-image"
	btrfs-image -r test.img.btrfs-image $dest || \
		_fail "failed to extract leaf_corrupt_no_data_ext.btrfs-image"

	# leaf at 20832256 contains no regular data extent, clear its csum to
	# corrupt the leaf.
	dd if=/dev/zero of=$dest bs=1 count=32 conv=notrunc seek=20832256 \
		1>/dev/null 2>&1
}

check_inode()
{
	path=$1
	ino=$2
	size=$3
	mode=$4
	name=$5

	# Check whether the inode exists
	exists=$($sudo find $path -inum $ino)
	if [ -z "$exists" ]; then
		_fail "inode $ino not recovered correctly"
	fi

	# Check inode type
	found_mode=$(printf "%o" 0x$($sudo stat $exists -c %f))
	if [ $found_mode -ne $mode ]; then
		echo "$found_mode"
		_fail "inode $ino modes not recovered"
	fi

	# Check inode size
	found_size=$($sudo stat $exists -c %s)
	if [ $mode -ne 41700 -a $found_size -ne $size ]; then
		_fail "inode $ino size not recovered correctly"
	fi

	# Check inode name
	if [ "$(basename $exists)" != "$name" ]; then
		_fail "inode $ino name not recovered correctly"
	else
		return 0
	fi
}

# Check salvaged data in the recovered image
check_leaf_corrupt_no_data_ext()
{
	image=$1
	if [ $have_root_helper -ne 1 ]; then
		echo "     [NOTRUN] root privileges needed to verify recovery"
		exit 0
	fi
	if [ -z $TEST_MNT ]; then
		echo "\$TEST_MNT not set, use $(pwd)/tmp as fallback"
		TEST_MNT="$(pwd)/tmp"
	fi
	mkdir -p $TEST_MNT || _fail "failed to create mount point"
	$sudo mount $image -o ro $TEST_MNT

	i=0
	while [ $i -lt ${#leaf_no_data_ext_list[@]} ]; do
		check_inode $TEST_MNT/lost+found \
			    ${leaf_no_data_ext_list[i]} \
			    ${leaf_no_data_ext_list[i + 1]} \
			    ${leaf_no_data_ext_list[i + 2]} \
			    ${leaf_no_data_ext_list[i + 3]} \
			    ${leaf_no_data_ext_list[i + 4]}
			    ((i+=4))
	done
	$sudo umount $TEST_MNT
}

setup_root_helper

generate_leaf_corrupt_no_data_ext test.img
check_image test.img
check_leaf_corrupt_no_data_ext test.img

rm test.img
rm test.img.btrfs-image
# Not used, its function is the same as generate_leaf_corrupt_no_data_ext()
rm generate_image.sh
