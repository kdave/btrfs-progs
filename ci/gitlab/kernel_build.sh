#!/usr/bin/env bash
#
# Setup BTRFS kernel options and build kernel

set -x

apt-get update
apt-get -y install build-essential libncurses-dev bison flex libssl-dev libelf-dev unzip wget bc

# Build kernel
wget https://github.com/kdave/btrfs-devel/archive/misc-next.zip
unzip -qq  misc-next.zip
cd btrfs-devel-misc-next/ && make x86_64_defconfig && make kvmconfig

# BTRFS specific entries
cat <<EOF >> .config
CONFIG_BTRFS_FS=y
CONFIG_BTRFS_FS_POSIX_ACL=y
CONFIG_BTRFS_FS_CHECK_INTEGRITY=n
CONFIG_BTRFS_FS_RUN_SANITY_TESTS=n
CONFIG_BTRFS_DEBUG=y
CONFIG_BTRFS_ASSERT=y
CONFIG_BTRFS_FS_REF_VERIFY=y
CONFIG_RAID6_PQ_BENCHMARK=y
CONFIG_LIBCRC32C=y
EOF

make -j8

# Store file to shared dir
cp -v arch/x86/boot/bzImage /repo
