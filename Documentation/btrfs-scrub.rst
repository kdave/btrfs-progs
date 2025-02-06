btrfs-scrub(8)
==============

SYNOPSIS
--------

**btrfs scrub** <subcommand> <args>

DESCRIPTION
-----------

.. include:: ch-scrub-intro.rst

SUBCOMMAND
----------

cancel <path>|<device>
        If a scrub is running on the filesystem identified by *path* or
        *device*, cancel it.

        If a *device* is specified, the corresponding filesystem is found and
        :command:`btrfs scrub cancel` behaves as if it was called on that filesystem.
        The progress is saved in the status file so :command:`btrfs scrub resume` can
        continue from the last position.

.. _man-scrub-limit:

limit [options] <path>
	Show or set scrub limits on devices of the given filesystem.

        ``Options``

        -d|--devid DEVID
                select the device by DEVID to apply the limit
        -l|--limit SIZE
                set the limit of the device to SIZE (size units with suffix),
                or 0 to reset to *unlimited*
	-a|--all
                apply the limit to all devices

        --raw
                print all numbers raw values in bytes without the *B* suffix
        --human-readable
                print human friendly numbers, base 1024, this is the default
        --iec
                select the 1024 base for the following options, according to
                the IEC standard
        --si
                select the 1000 base for the following options, according to the SI standard
        --kbytes
                show sizes in KiB, or kB with --si
        --mbytes
                show sizes in MiB, or MB with --si
        --gbytes
                show sizes in GiB, or GB with --si
        --tbytes
                show sizes in TiB, or TB with --si

resume [-BdqrR] <path>|<device>
        Resume a cancelled or interrupted scrub on the filesystem identified by
        *path* or on a given *device*. The starting point is read from the
        status file if it exists.

        This does not start a new scrub if the last scrub finished successfully.

        ``Options``

        see :command:`scrub start`.

.. _man-scrub-start:

start [options] <path>|<device>
        Start a scrub on all devices of the mounted filesystem identified by
        *path* or on a single *device*. If a scrub is already running, the new
        one will not start. A device of an unmounted filesystem cannot be
        scrubbed this way.

        Without options, scrub is started as a background process. The
        automatic repairs of damaged copies are performed by default for block
        group profiles with redundancy. No-repair can be enabled by option *-r*.

        ``Options``

        -B
                do not background and print scrub statistics when finished
        -d
                print separate statistics for each device of the filesystem
                (*-B* only) at the end
        -r
                run in read-only mode, do not attempt to correct anything, can
                be run on a read-only filesystem

                Note that a read-only scrub on a read-write filesystem can
                still cause writes into the filesystem due to some internal
                limitations.  Only a read-only scrub on a read-only filesystem
                can avoid writes from scrub.
        -R
                raw print mode, print full data instead of summary
	--limit <limit>
		set the scrub throughput limit for each device.

		If the scrub is for the whole fs, it's the same as
		:command:`btrfs scrub limit -a -l <value>`.
		If the scrub is for a single device, it's the same as
		:command:`btrfs scrub limit -d <devid> -l <value>`.

		The value is bytes per second, and accepts the usual KMGT prefixes.
		After the scrub is finished, the throughput limit will be reset to
		the old value of each device.
        -f
                force starting new scrub even if a scrub is already running,
                this can useful when scrub status file is damaged and reports a
                running scrub although it is not, but should not normally be
                necessary

        ``Deprecated options``

        -c <ioprio_class>
                set IO priority class (see :manref:`ionice(1)` manual page) if the IO
                scheduler configured for the device supports ionice. This is
                only supported by BFQ or Kyber but is *not* supported by
                mq-deadline. Please read the section about
                :docref:`IO limiting <btrfs-scrub:scrub-io-limiting>`.
        -n <ioprio_classdata>
                set IO priority classdata (see :manref:`ionice(1)` manpage)
        -q
                (deprecated) alias for global *-q* option

status [options] <path>|<device>
        Show status of a running scrub for the filesystem identified by *path*
        or for the specified *device*.

        If no scrub is running, show statistics of the last finished or
        cancelled scrub for that filesystem or device.

        ``Options``

        -d
                print separate statistics for each device of the filesystem
        -R
                print all raw statistics without postprocessing as returned by
                the status ioctl
        --raw
                print all numbers raw values in bytes without the *B* suffix
        --human-readable
                print human friendly numbers, base 1024, this is the default
        --iec
                select the 1024 base for the following options, according to
                the IEC standard
        --si
                select the 1000 base for the following options, according to the SI standard
        --kbytes
                show sizes in KiB, or kB with --si
        --mbytes
                show sizes in MiB, or MB with --si
        --gbytes
                show sizes in GiB, or GB with --si
        --tbytes
                show sizes in TiB, or TB with --si

        A status on a filesystem without any error looks like the following:

        .. code-block:: none

           # btrfs scrub start /
           # btrfs scrub status /
           UUID:             76fac721-2294-4f89-a1af-620cde7a1980
           Scrub started:    Wed Apr 10 12:34:56 2023
           Status:           running
           Duration:         0:00:05
           Time left:        0:00:05
           ETA:              Wed Apr 10 12:35:01 2023
           Total to scrub:   28.32GiB
           Bytes scrubbed:   13.76GiB  (48.59%)
           Rate:             2.75GiB/s
           Error summary:    no errors found

        With some errors found:

        .. code-block:: none

           Error summary:    csum=72
             Corrected:      2
             Uncorrectable:  72
             Unverified:     0

        *  *Corrected* -- number of bad blocks that were repaired from another copy
        *  *Uncorrectable* -- errors detected at read time but not possible to repair from other copy
        *  *Unverified* -- transient errors, first read failed but a retry
           succeeded, may be affected by lower layers that group or split IO requests
        *  *Error summary* -- followed by a more detailed list of errors found

           *  *csum* -- checksum mismatch
           *  *super* -- super block errors, unless the error is fixed
              immediately, the next commit will overwrite superblock
           *  *verify* -- metadata block header errors
           *  *read* -- blocks can't be read due to IO errors

        It's possible to set a per-device limit via file
        :file:`sysfs/fs/btrfs/FSID/devinfo/scrub_speed_max`. In that case
        the limit is printed on the *Rate:* line if option *-d* is specified,
        or without it on a single-device filesystem.  Read more about tat in
        section about :docref:`scrub IO limiting <btrfs-scrub:scrub-io-limiting>`.

        .. code-block:: none

           Rate:             989.0MiB/s (limit 1.0G/s)

        On a multi-device filesystem with at least one device limit the
        overall stats cannot print the limit without *-d* so there's a not that
        some limits are set:

        .. code-block:: none

           Rate:             36.37MiB/s (some device limits set)

EXIT STATUS
-----------

**btrfs scrub** returns a zero exit status if it succeeds. Non zero is
returned in case of failure:

1
        scrub couldn't be performed
2
        there is nothing to resume
3
        scrub found uncorrectable errors

AVAILABILITY
------------

**btrfs** is part of btrfs-progs.  Please refer to the documentation at
`https://btrfs.readthedocs.io <https://btrfs.readthedocs.io>`_.

SEE ALSO
--------

:doc:`mkfs.btrfs`
