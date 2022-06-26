From kernel 3.3 onwards, BTRFS balance can limit its action to a subset of the
whole filesystem, and can be used to change the replication configuration (e.g.
convert data from ``single`` to ``RAID1``). 

Balance can be limited to a group with target block:

* ``-d`` for data blocks.
* ``-m`` for metadata blocks.
* ``-s`` for system blocks.

To limit filter to a block type, begin filter with the option. Example: ``-dusage=10``

Multiple block type can be spesifed per balance.

A filter has the following structure: ``[target block]filter[=params][,filter=...]``

To combind multi filters use ``,``, without spacing. Example: ``-dconvert=raid1,soft``

BTRFS can have different profiles on a single disk or the same profile on multiple disks.

The main reason why you want to have different profiles for data and metadata 
is to provide additional protection of the filesystem's metadata when disks fail, 
since a single sector of unrecoverable metadata will break the filesystem, 
while a single sector of lost data can be trivially recovered by deleting the broken file. 

Before changing profiles, make sure there is enough unallocated space on
existing drives to create new metadata block groups (for filesystems
over 50GB, this is ``1GB * (number_of_devices + 2))``.

default profile on BTRFS is:

* Data: ``single`` 
* metadata: 
        * single-disk: ``dup``
        * multiple devices: ``raid1``


The available filter types are:

profiles=<profiles>
        Balances only block groups with the given profiles. Parameters
        are a list of profile names separated by ``|`` (pipe).

usage=<percent>, usage=<range>
        Balances only block groups with usage under the given percentage. The
        value of 0 is allowed and will clean up completely unused block groups, this
        should not require any new work space allocated. You may want to use *usage=0*
        in case balance is returning ENOSPC and your filesystem is not too full.

        The argument may be a single value or a range. The single value ``N`` means *at
        most N percent used*, equivalent to ``..N`` range syntax. Kernels prior to 4.4
        accept only the single value format.
        The minimum range boundary is inclusive, maximum is exclusive.

devid=<id>
        Balances only block groups which have at least one chunk on the given
        device. To list devices with ids use ``BTRFS filesystem show``.

drange=<range>
        Balance only block groups which overlap with the given byte range on any
        device. Use in conjunction with ``devid`` to filter on a specific device. The
        parameter is a range specified as ``start..end``.

vrange=<range>
        Balance only block groups which overlap with the given byte range in the
        filesystem's internal virtual address space. This is the address space that
        most reports from btrfs in the kernel log use. The parameter is a range
        specified as ``start..end``.

convert=<profile>
        Convert each selected block group to the given profile name identified by
        parameters.

        .. note::
                Starting with kernel 4.5, the ``data`` chunks can be converted to/from the
                ``DUP`` profile on a single device.

        .. note::
                Starting with kernel 4.6, all profiles can be converted to/from ``DUP`` on
                multi-device filesystems.

limit=<number>, limit=<range>
        Process only given number of chunks, after all filters are applied. This can be
        used to specifically target a chunk in connection with other filters (``drange``,
        ``vrange``) or just simply limit the amount of work done by a single balance run.

        The argument may be a single value or a range. The single value ``N`` means *at
        most N chunks*, equivalent to ``..N`` range syntax. Kernels prior to 4.4 accept
        only the single value format.  The range minimum and maximum are inclusive.

stripes=<range>
        Balance only block groups which have the given number of stripes. The parameter
        is a range specified as ``start..end``. Makes sense for block group profiles that
        utilize striping, ie. RAID0/10/5/6.  The range minimum and maximum are
        inclusive.

soft
        Takes no parameters. Only has meaning when converting between profiles, or 
        When doing convert from one profile to another and soft mode is on,
        chunks that already have the target profile are left untouched.
        This is useful e.g. when half of the filesystem was converted earlier but got
        cancelled.

        The soft mode switch is (like every other filter) per-type.
        For example, this means that we can convert metadata chunks the "hard" way
        while converting data chunks selectively with soft switch.

Profile names, used in ``profiles`` and ``convert`` are one of: 

* ``raid0``
* ``raid1``
* ``raid1c3``
* ``raid1c4``
* ``raid10``
* ``raid5``
* ``raid6``
* ``dup``
* ``single``

The mixed data/metadata profiles can be converted in the same way, but it's conversion
between mixed and non-mixed is not implemented. For the constraints of the
profiles please refer to ``mkfs.btrfs(8)``, section *PROFILES*.
