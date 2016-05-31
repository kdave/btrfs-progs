#!/bin/bash
#
# convert ext2/3/4 images to btrfs images, and make sure the results are
# clean.
#

unset TOP
unset LANG
LANG=C
SCRIPT_DIR=$(dirname $(readlink -f $0))
TOP=$(readlink -f $SCRIPT_DIR/../)
RESULTS="$TOP/tests/convert-tests-results.txt"
# how many files to create.
DATASET_SIZE=50

source $TOP/tests/common

rm -f $RESULTS

setup_root_helper
prepare_test_dev 512M

CHECKSUMTMP=$(mktemp --tmpdir btrfs-progs-convert.XXXXXXXXXX)

generate_dataset() {

	dataset_type="$1"
	dirpath=$TEST_MNT/$dataset_type
	run_check $SUDO_HELPER mkdir -p $dirpath

	case $dataset_type in
		small)
			for num in $(seq 1 $DATASET_SIZE); do
				run_check $SUDO_HELPER dd if=/dev/urandom of=$dirpath/$dataset_type.$num bs=10K \
				count=1 >/dev/null 2>&1
			done
			;;

		hardlink)
			for num in $(seq 1 $DATASET_SIZE); do
				run_check $SUDO_HELPER touch $dirpath/$dataset_type.$num
				run_check $SUDO_HELPER ln $dirpath/$dataset_type.$num $dirpath/hlink.$num
			done
			;;

		symlink)
			for num in $(seq 1 $DATASET_SIZE); do
				run_check $SUDO_HELPER touch $dirpath/$dataset_type.$num
				run_check $SUDO_HELPER ln -s $dirpath/$dataset_type.$num $dirpath/slink.$num
			done
			;;

		brokenlink)
			for num in $(seq 1 $DATASET_SIZE); do
				run_check $SUDO_HELPER ln -s $dirpath/$dataset_type.$num $dirpath/blink.$num
			done
			;;

		perm)
			for modes in 777 775 755 750 700 666 664 644 640 600 444 440 400 000		\
				1777 1775 1755 1750 1700 1666 1664 1644 1640 1600 1444 1440 1400 1000	\
				2777 2775 2755 2750 2700 2666 2664 2644 2640 2600 2444 2440 2400 2000	\
				4777 4775 4755 4750 4700 4666 4664 4644 4640 4600 4444 4440 4400 4000; do
				if [[ "$modes" == *9* ]] || [[ "$modes" == *8* ]]
				then
					continue;
				else
					run_check $SUDO_HELPER touch $dirpath/$dataset_type.$modes
					run_check $SUDO_HELPER chmod $modes $dirpath/$dataset_type.$modes
				fi
			done
			;;

		sparse)
			for num in $(seq 1 $DATASET_SIZE); do
				run_check $SUDO_HELPER dd if=/dev/urandom of=$dirpath/$dataset_type.$num bs=10K \
				count=1 >/dev/null 2>&1
				run_check $SUDO_HELPER truncate -s 500K $dirpath/$dataset_type.$num
				run_check $SUDO_HELPER dd if=/dev/urandom of=$dirpath/$dataset_type.$num bs=10K \
				oflag=append conv=notrunc count=1 >/dev/null 2>&1
				run_check $SUDO_HELPER truncate -s 800K $dirpath/$dataset_type.$num
			done
			;;

		acls)
			for num in $(seq 1 $DATASET_SIZE); do
				run_check $SUDO_HELPER touch $dirpath/$dataset_type.$num
				run_check $SUDO_HELPER setfacl -m "u:root:x" $dirpath/$dataset_type.$num
				run_check $SUDO_HELPER setfattr -n user.foo -v bar$num $dirpath/$dataset_type.$num
			done
			;;
	esac
}

populate_fs() {

        for dataset_type in 'small' 'hardlink' 'symlink' 'brokenlink' 'perm' 'sparse' 'acls'; do
		generate_dataset "$dataset_type"
	done
}

convert_test() {
	local features
	local nodesize

	features="$1"
	shift

	if [ -z "$features" ]; then
		echo "    [TEST/conv]   $1, btrfs defaults"
	else
		echo "    [TEST/conv]   $1, btrfs $features"
	fi
	nodesize=$2
	shift 2
	echo "creating ext image with: $*" >> $RESULTS
	# TEST_DEV not removed as the file might have special permissions, eg.
	# when test image is on NFS and would not be writable for root
	run_check truncate -s 0 $TEST_DEV
	# 256MB is the smallest acceptable btrfs image.
	run_check truncate -s 512M $TEST_DEV
	run_check $* -F $TEST_DEV

	# create a file to check btrfs-convert can convert regular file
	# correct
	run_check_mount_test_dev
	populate_fs
	run_check $SUDO_HELPER dd if=/dev/zero of=$TEST_MNT/test bs=$nodesize \
		count=1 >/dev/null 2>&1
	run_check_stdout $SUDO_HELPER find $TEST_MNT -type f ! -name 'image' -exec md5sum {} \+ > $CHECKSUMTMP
	run_check_umount_test_dev

	run_check $TOP/btrfs-convert ${features:+-O "$features"} -N "$nodesize" $TEST_DEV
	run_check $TOP/btrfs check $TEST_DEV
	run_check $TOP/btrfs-show-super -Ffa $TEST_DEV

	run_check_mount_test_dev
	run_check_stdout $SUDO_HELPER md5sum -c $CHECKSUMTMP |
		grep -q 'FAILED' && _fail "file validation failed."
	run_check_umount_test_dev

	run_check $TOP/btrfs-convert --rollback $TEST_DEV
	run_check fsck -n -t ext2,ext3,ext4 $TEST_DEV
}

if ! [ -z "$TEST" ]; then
	echo "    [TEST/conv]   skipped all convert tests, TEST=$TEST"
	exit 0
fi

for feature in '' 'extref' 'skinny-metadata' 'no-holes'; do
	convert_test "$feature" "ext2 4k nodesize" 4096 mke2fs -b 4096
	convert_test "$feature" "ext3 4k nodesize" 4096 mke2fs -j -b 4096
	convert_test "$feature" "ext4 4k nodesize" 4096 mke2fs -t ext4 -b 4096
	convert_test "$feature" "ext2 8k nodesize" 8192 mke2fs -b 4096
	convert_test "$feature" "ext3 8k nodesize" 8192 mke2fs -j -b 4096
	convert_test "$feature" "ext4 8k nodesize" 8192 mke2fs -t ext4 -b 4096
	convert_test "$feature" "ext2 16k nodesize" 16384 mke2fs -b 4096
	convert_test "$feature" "ext3 16k nodesize" 16384 mke2fs -j -b 4096
	convert_test "$feature" "ext4 16k nodesize" 16384 mke2fs -t ext4 -b 4096
	convert_test "$feature" "ext2 32k nodesize" 32768 mke2fs -b 4096
	convert_test "$feature" "ext3 32k nodesize" 32768 mke2fs -j -b 4096
	convert_test "$feature" "ext4 32k nodesize" 32768 mke2fs -t ext4 -b 4096
	convert_test "$feature" "ext2 64k nodesize" 65536 mke2fs -b 4096
	convert_test "$feature" "ext3 64k nodesize" 65536 mke2fs -j -b 4096
	convert_test "$feature" "ext4 64k nodesize" 65536 mke2fs -t ext4 -b 4096
done

rm $CHECKSUMTMP
