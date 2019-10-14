#!/usr/bin/env bash
#
# Install and start qemu instance with custom kernel while exporting
# btrfs-progs src over 9p
#
set -x

qemu-system-x86_64 -m 512 -nographic -kernel /repo/bzImage \
	-drive file=/repo/qemu-image.img,index=0,media=disk,format=raw \
	-fsdev local,id=btrfs-progs,path=/repo,security_model=mapped \
	-device virtio-9p-pci,fsdev=btrfs-progs,mount_tag=btrfs-progs \
	-append "console=tty1 root=/dev/sda rw"
