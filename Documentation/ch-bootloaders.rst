GRUB2 (https://www.gnu.org/software/grub) has the most advanced support of
booting from BTRFS with respect to features.

U-boot (https://www.denx.de/wiki/U-Boot/) has decent support for booting but
not all BTRFS features are implemented, check the documentation.

EXTLINUX (from the https://syslinux.org project) has limited support for BTRFS
boot and hasn't been updated for for a long time so is not recommended as
bootloader.

In general, the first 1MiB on each device is unused with the exception of
primary superblock that is on the offset 64KiB and spans 4KiB. The rest can be
freely used by bootloaders or for other system information. Note that booting
from a filesystem on :doc:`zoned device<Zoned-mode>` is not supported.
