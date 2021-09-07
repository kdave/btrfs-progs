#!/bin/bash
# mkfs.btrfs must fail on a thin provision device with very small backing size
# and big virtual size.

source "$TEST_TOP/common"

check_prereq mkfs.btrfs
check_global_prereq udevadm
check_dm_target_support linear
check_dm_target_support thin thin-pool

setup_root_helper
prepare_test_dev

# Backing data dev
DMTHIN_DATA_NAME="btrfs-progs-thin-data"
DMTHIN_DATA_DEV="/dev/mapper/$DMTHIN_DATA_NAME"
# Backing metadata dev
DMTHIN_META_NAME="btrfs-progs-thin-meta"
DMTHIN_META_DEV="/dev/mapper/$DMTHIN_META_NAME"
# Backing pool dev (combination of above)
DMTHIN_POOL_NAME="btrfs-progs-thin-pool"
DMTHIN_POOL_DEV="/dev/mapper/$DMTHIN_POOL_NAME"
# Thin volume
DMTHIN_VOL_NAME="btrfs-progs-thin-vol"
DMTHIN_VOL_DEV="/dev/mapper/$DMTHIN_VOL_NAME"

dmthin_cleanup()
{
	# wait for device to be fully settled
	run_check $SUDO_HELPER udevadm settle
	run_check $SUDO_HELPER dmsetup remove "$DMTHIN_VOL_NAME"
	run_check $SUDO_HELPER dmsetup remove "$DMTHIN_POOL_NAME"
	run_check $SUDO_HELPER dmsetup remove "$DMTHIN_META_NAME"
	run_check $SUDO_HELPER dmsetup remove "$DMTHIN_DATA_NAME"
}

sector_size=512		  # in bytes
data_dev_size=$((1 * 1024 * 1024 / $sector_size))   # 1M
virtual_size=$((1 * 1024 * 1024 * 1024 * 1024 / $sector_size))  # 1T
cluster_size=1024	  # 512k in sectors
low_water=$((104857600 / $cluster_size/ $sector_size))  # 100M / $cluster_size, in sectors

# Need to make linear metadata and data devs.  From kernel docs:
# As a guide, we suggest you calculate the number of bytes to use in the
# metadata device as 48 * $data_dev_size / $data_block_size but round it up
# to 2MB (4096 sectors) if the answer is smaller.
# So do that:
meta_dev_size=$((48 * $data_dev_size / $cluster_size))
if [ "$meta_dev_size" -lt "4096" ]; then
        meta_dev_size=4096      # 2MB
fi

meta_dev_offset=0
total_data_dev_size=$(($meta_dev_offset + $meta_dev_size + $data_dev_size))

run_check truncate -s0 img
chmod a+w img
run_check truncate -s"$(($total_data_dev_size * $sector_size))" img

dm_backing_dev=`run_check_stdout $SUDO_HELPER losetup --find --show img`

if ! [ -b "$dm_backing_dev" ]; then
	_fail "cannot create backing device"
fi

# Metadata device
DMTHIN_META_TABLE="0 $meta_dev_size linear $dm_backing_dev $meta_dev_offset"
run_check $SUDO_HELPER dmsetup create "$DMTHIN_META_NAME" --table "$DMTHIN_META_TABLE"

# Data device
data_dev_offset=$((meta_dev_offset + $meta_dev_size))
DMTHIN_DATA_TABLE="0 $data_dev_size linear $dm_backing_dev $data_dev_offset"
run_check $SUDO_HELPER dmsetup create "$DMTHIN_DATA_NAME" --table "$DMTHIN_DATA_TABLE"

# Zap the pool metadata dev
run_check $SUDO_HELPER dd if=/dev/zero of="$DMTHIN_META_DEV" bs=4096 count=1

# Thin pool
# "start length thin-pool metadata_dev data_dev data_block_size low_water_mark"
DMTHIN_POOL_TABLE="0 $data_dev_size thin-pool $DMTHIN_META_DEV $DMTHIN_DATA_DEV $cluster_size $low_water"
run_check $SUDO_HELPER dmsetup create "$DMTHIN_POOL_NAME" --table "$DMTHIN_POOL_TABLE"

# Thin volume
pool_id=$RANDOM
run_check $SUDO_HELPER dmsetup message "$DMTHIN_POOL_DEV" 0 "create_thin $pool_id"

# start length thin pool_dev dev_id [external_origin_dev]
DMTHIN_VOL_TABLE="0 $virtual_size thin $DMTHIN_POOL_DEV $pool_id"
run_check $SUDO_HELPER dmsetup create "$DMTHIN_VOL_NAME" --table "$DMTHIN_VOL_TABLE"

# mkfs.btrfs should fail due to the small backing device, the initial discard
# is disabled
run_mustfail "should fail for small backing size thin provision device" \
	     $SUDO_HELPER "$TOP/mkfs.btrfs" -K -f "$DMTHIN_VOL_DEV"

dmthin_cleanup
run_mayfail $SUDO_HELPER losetup -d "$dm_backing_dev"
rm -- img
