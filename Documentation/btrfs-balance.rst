btrfs-balance(8)
================

SYNOPSIS
--------

**btrfs balance** <subcommand> <args>

DESCRIPTION
-----------

.. include:: ch-balance-intro.rst

SUBCOMMAND
----------

cancel <path>
        cancels a running or paused balance, the command will block and wait until the
        current block group being processed completes

        Since kernel 5.7 the response time of the cancellation is significantly
        improved, on older kernels it might take a long time until currently
        processed chunk is completely finished.

pause <path>
        pause running balance operation, this will store the state of the balance
        progress and used filters to the filesystem

resume <path>
        resume interrupted balance, the balance status must be stored on the filesystem
        from previous run, e.g. after it was paused or forcibly interrupted and mounted
        again with *skip_balance*

start [options] <path>
        start the balance operation according to the specified filters, without any filters
        the data and metadata from the whole filesystem are moved. The process runs in
        the foreground.

        .. note::
                The balance command without filters will basically move everything in the
                filesystem to a new physical location on devices (i.e. it does not affect the
                logical properties of file extents like offsets within files and extent
                sharing).  The run time is potentially very long, depending on the filesystem
                size. To prevent starting a full balance by accident, the user is warned and
                has a few seconds to cancel the operation before it starts.  The warning and
                delay can be skipped with *--full-balance* option.

        Please note that the filters must be written together with the *-d*, *-m* and
        *-s* options, because they're optional and bare *-d* and *-m* also work and
        mean no filters.

        .. note::
                When the target profile for conversion filter is *raid5* or *raid6*,
                there's a safety timeout of 10 seconds to warn users about the status of the feature

        ``Options``

        -d[<filters>]
                act on data block groups, see *FILTERS* section for details about *filters*
        -m[<filters>]
                act on metadata chunks, see *FILTERS* section for details about *filters*
        -s[<filters>]
                act on system chunks (requires *-f*), see *FILTERS* section for details about *filters*.

        -f
                force a reduction of metadata integrity, e.g. when going from *raid1* to
                *single*, or skip safety timeout when the target conversion profile is *raid5*
                or *raid6*

        --background|--bg
                run the balance operation asynchronously in the background, uses ``fork(2)`` to
                start the process that calls the kernel ioctl

        --enqueue
                wait if there's another exclusive operation running, otherwise continue
        -v
                (deprecated) alias for global '-v' option

status [-v] <path>
        Show status of running or paused balance.

        ``Options``

        -v
                (deprecated) alias for global *-v* option

FILTERS
-------

.. include:: ch-balance-filters.rst

ENOSPC
------

The way balance operates, it usually needs to temporarily create a new block
group and move the old data there, before the old block group can be removed.
For that it needs the work space, otherwise it fails for ENOSPC reasons.
This is not the same ENOSPC as if the free space is exhausted. This refers to
the space on the level of block groups, which are bigger parts of the filesystem
that contain many file extents.

The free work space can be calculated from the output of the **btrfs filesystem show**
command:

.. code-block:: none

   Label: 'BTRFS'  uuid: 8a9d72cd-ead3-469d-b371-9c7203276265
	   Total devices 2 FS bytes used 77.03GiB
	   devid    1 size 53.90GiB used 51.90GiB path /dev/sdc2
	   devid    2 size 53.90GiB used 51.90GiB path /dev/sde1

*size* - *used* = *free work space*

*53.90GiB* - *51.90GiB* = *2.00GiB*

An example of a filter that does not require workspace is *usage=0*. This will
scan through all unused block groups of a given type and will reclaim the
space. After that it might be possible to run other filters.

**CONVERSIONS ON MULTIPLE DEVICES**

Conversion to profiles based on striping (RAID0, RAID5/6) require the work
space on each device. An interrupted balance may leave partially filled block
groups that consume the work space.

EXAMPLES
--------

A more comprehensive example when going from one to multiple devices, and back,
can be found in section *TYPICAL USECASES* of :doc:`btrfs-device(8)<btrfs-device>`.

MAKING BLOCK GROUP LAYOUT MORE COMPACT
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The layout of block groups is not normally visible; most tools report only
summarized numbers of free or used space, but there are still some hints
provided.

Let's use the following real life example and start with the output:

.. code-block:: none

        $ btrfs filesystem df /path
        Data, single: total=75.81GiB, used=64.44GiB
        System, RAID1: total=32.00MiB, used=20.00KiB
        Metadata, RAID1: total=15.87GiB, used=8.84GiB
        GlobalReserve, single: total=512.00MiB, used=0.00B

Roughly calculating for data, *75G - 64G = 11G*, the used/total ratio is
about *85%*. How can we can interpret that:

* chunks are filled by 85% on average, i.e. the *usage* filter with anything
  smaller than 85 will likely not affect anything
* in a more realistic scenario, the space is distributed unevenly, we can
  assume there are completely used chunks and the remaining are partially filled

Compacting the layout could be used on both. In the former case it would spread
data of a given chunk to the others and removing it. Here we can estimate that
roughly 850 MiB of data have to be moved (85% of a 1 GiB chunk).

In the latter case, targeting the partially used chunks will have to move less
data and thus will be faster. A typical filter command would look like:

.. code-block:: none

        # btrfs balance start -dusage=50 /path
        Done, had to relocate 2 out of 97 chunks

        $ btrfs filesystem df /path
        Data, single: total=74.03GiB, used=64.43GiB
        System, RAID1: total=32.00MiB, used=20.00KiB
        Metadata, RAID1: total=15.87GiB, used=8.84GiB
        GlobalReserve, single: total=512.00MiB, used=0.00B

As you can see, the *total* amount of data is decreased by just 1 GiB, which is
an expected result. Let's see what will happen when we increase the estimated
usage filter.

.. code-block:: none

        # btrfs balance start -dusage=85 /path
        Done, had to relocate 13 out of 95 chunks

        $ btrfs filesystem df /path
        Data, single: total=68.03GiB, used=64.43GiB
        System, RAID1: total=32.00MiB, used=20.00KiB
        Metadata, RAID1: total=15.87GiB, used=8.85GiB
        GlobalReserve, single: total=512.00MiB, used=0.00B

Now the used/total ratio is about 94% and we moved about *74G - 68G = 6G* of
data to the remaining block groups, i.e. the 6GiB are now free of filesystem
structures, and can be reused for new data or metadata block groups.

We can do a similar exercise with the metadata block groups, but this should
not typically be necessary, unless the used/total ratio is really off. Here
the ratio is roughly 50% but the difference as an absolute number is "a few
gigabytes", which can be considered normal for a workload with snapshots or
reflinks updated frequently.

.. code-block:: none

        # btrfs balance start -musage=50 /path
        Done, had to relocate 4 out of 89 chunks

        $ btrfs filesystem df /path
        Data, single: total=68.03GiB, used=64.43GiB
        System, RAID1: total=32.00MiB, used=20.00KiB
        Metadata, RAID1: total=14.87GiB, used=8.85GiB
        GlobalReserve, single: total=512.00MiB, used=0.00B

Just 1 GiB decrease, which possibly means there are block groups with good
utilization. Making the metadata layout more compact would in turn require
updating more metadata structures, i.e. lots of IO. As running out of metadata
space is a more severe problem, it's not necessary to keep the utilization
ratio too high. For the purpose of this example, let's see the effects of
further compaction:

.. code-block:: none

        # btrfs balance start -musage=70 /path
        Done, had to relocate 13 out of 88 chunks

        $ btrfs filesystem df .
        Data, single: total=68.03GiB, used=64.43GiB
        System, RAID1: total=32.00MiB, used=20.00KiB
        Metadata, RAID1: total=11.97GiB, used=8.83GiB
        GlobalReserve, single: total=512.00MiB, used=0.00B

GETTING RID OF COMPLETELY UNUSED BLOCK GROUPS
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Normally the balance operation needs a work space, to temporarily move the
data before the old block groups gets removed. If there's no work space, it
ends with *no space left*.

There's a special case when the block groups are completely unused, possibly
left after removing lots of files or deleting snapshots. Removing empty block
groups is automatic since 3.18. The same can be achieved manually with a
notable exception that this operation does not require the work space. Thus it
can be used to reclaim unused block groups to make it available.

.. code-block:: bash

        # btrfs balance start -dusage=0 /path

This should lead to decrease in the *total* numbers in the **btrfs filesystem df** output.

EXIT STATUS
-----------

Unless indicated otherwise below, all **btrfs balance** subcommands
return a zero exit status if they succeed, and non zero in case of
failure.

The **pause**, **cancel**, and **resume** subcommands exit with a status of
**2** if they fail because a balance operation was not running.

The **status** subcommand exits with a status of **0** if a balance
operation is not running, **1** if the command-line usage is incorrect
or a balance operation is still running, and **2** on other errors.

AVAILABILITY
------------

**btrfs** is part of btrfs-progs.  Please refer to the documentation at
https://btrfs.readthedocs.io or wiki http://btrfs.wiki.kernel.org for further
information.

SEE ALSO
--------

:doc:`mkfs.btrfs(8)<mkfs.btrfs>`,
:doc:`btrfs-device(8)<btrfs-device>`
