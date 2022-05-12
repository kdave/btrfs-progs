GRUB2 (https://www.gnu.org/software/grub) has the most advanced support of
booting from BTRFS with respect to features.

U-boot (https://www.denx.de/wiki/U-Boot/) has decent support for booting but
not all BTRFS features are implemented, check the documentation.

EXTLINUX (from the https://syslinux.org project) can boot but does not support
all features. Please check the upstream documentation before you use it.

The first 1MiB on each device is unused with the exception of primary
superblock that is on the offset 64KiB and spans 4KiB.
