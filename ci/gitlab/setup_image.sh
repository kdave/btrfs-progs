#!/usr/bin/env bash
#
# Setup debian image via debootstrap and include systemd service file.
set -x

apt-get update
apt-get -y install debootstrap wget unzip

# Setup rootfs
IMG="/qemu-image.img"
DIR="/target"
truncate -s2G $IMG
mkfs.ext4 $IMG
mkdir -p $DIR
for i in {0..7};do
mknod -m 0660 "/dev/loop$i" b 7 "$i"
done

# mount the image file
mount -o loop $IMG $DIR

# Install required pacakges
debootstrap --arch=amd64  --include=git,autoconf,automake,gcc,make,pkg-config,e2fslibs-dev,libblkid-dev,zlib1g-dev,liblzo2-dev,asciidoc,xmlto,libzstd-dev,python3.5,python3.5-dev,python3-dev,python3-setuptools,python-setuptools,xz-utils,acl,attr stretch $DIR http://ftp.de.debian.org/debian/

## Setup 9p mount
echo "btrfs-progs /mnt           9p             trans=virtio    0       0" > $DIR/etc/fstab

# Setup autologin
sed -i 's/9600/9600 --autologin root/g' $DIR/lib/systemd/system/serial-getty@.service

# Setup systemd service
cp -v /repo/ci/gitlab/build_or_run_btrfs-progs.sh $DIR/usr/bin/
cp -v /repo/ci/gitlab/btrfs-progs-tests.service $DIR/etc/systemd/system/

## Enable service
ln -s $DIR/etc/systemd/system/btrfs-progs-tests.service $DIR/etc/systemd/system/getty.target.wants/btrfs-progs-tests.service 

cd /
umount $DIR
rmdir $DIR

cp -v $IMG /repo
