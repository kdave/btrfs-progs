# Btrfs-progs tests

A testsuite covering functionality of btrfs-progs, ie. the checker, image, mkfs
and similar tools. There are no special requirements on kernel features, the
tests build on top of the core functionality like snapshots and device
management. In some cases optional features are turned on by mkfs and the
filesystem image could be mounted, such tests might fail if there's lack of
support.

## Quick start

Run the tests from the top directory:

```shell
$ make test
$ make test-fsck
$ make test-convert
```

or selectively from the `tests/` directory:

```shell
$ ./fsck-tests.sh
$ ./misc-tests.sh
```

The verbose output of the tests is logged into a file named after the test
category, eg. `fsck-tests-results.txt`.

## Selective testing

The test are prefixed by a number for ordering and uniqueness. To run a
particular test use:

```shell
$ make TEST=MASK test
```

where `MASK` is a glob expression that will execute only tests
that match the MASK. Here the test number comes handy:

```shell
$ make TEST=001\* test-fsck
$ TEST=001\* ./fsck-tests.sh
```

will run the first test in fsck-tests subdirectory.


## Test structure

*tests/fsck-tests/:*

  * tests targeted at bugs that are fixable by fsck

*tests/convert-tests/:*

  * coverage tests of ext2/3/4 and btrfs-convert options

*tests/fuzz-tests/:*

  * collection of fuzzed or crafted images
  * tests that are supposed to run various utilities on the images and not
    crash

*tests/cli-tests/:*

  * tests for command line interface, option coverage, weird option combinations that should not work
  * not necessary to do any functional testing, could be rather lightweight
  * functional tests should go to to other test dirs
  * the driver script will only execute `./test.sh` in the test directory

*tests/misc-tests/:*

  * anything that does not fit to the above, the test driver script will only
    execute `./test.sh` in the test directory

*tests/common:*
*tests/common.convert:*

  * script with shell helpers, separated by functionality

*tests/test.img:*

  * default testing image, the file is never deleted by the scripts but
    truncated to 0 bytes, so it keeps it's permissions. It's eg. possible to
    host it on NFS, make it `chmod a+w` for root.


## Other tuning, environment variables

### Instrumentation

It's possible to wrap the tested commands to utilities that might do more
checking or catch failures at runtime. This can be done by setting the
`INSTRUMENT` environment variable:

```shell
INSTRUMENT=valgrind ./fuzz-tests.sh    # in tests/
make INSTRUMENT=valgrind test-fuzz     # in the top directory
```

The variable is prepended to the command *unquoted*, all sorts of shell tricks
are possible.

Note: instrumentation is not applied to privileged commands (anything that uses
the root helper).

### Verbosity

Setting the variable `TEST_LOG=tty` will print all commands executed by some of
the wrappers (`run_check` etc), other commands are not printed to the terminal
(but the full output is in the log).

### Permissions

Some commands require root privileges (to mount/umount, access loop devices).
It is assumed that `sudo` will work in some way (no password, password asked
and cached). Note that instrumentation is not applied in this case, for safety
reasons. You need to modify the test script instead.

### Cleanup

The tests are supposed to cleanup after themselves if they pass. In case of
failure, the rest of the tests are skipped and intermediate files, mounts and
loop devices are kept. This should help to investigate the test failure but at
least the mounts and loop devices need to be cleaned before the next run.

This is partially done by the script `clean-tests.sh`, you may want to check
the loop devices as they are managed on a per-test basis.

### Prototyping tests, quick tests

There's a script `test-console.sh` that will run shell commands in a loop and
logs the output with the testing environment set up.

## New test

1. Pick the category for the new test or fallback to `misc-tests` if not sure. For
an easy start copy an existing `test.sh` script from some test that might be
close to the purpose of your new test. The environment setup includes the
common scripts and/or prepares the test devices. Other scripts contain examples
how to do mkfs, mount, unmount, check, etc.

2. Use the highest unused number in the sequence, write a short descriptive title
and join by dashes `-`. This will become the directory name, eg. `012-subvolume-sync-must-wait`.

3. Write a short description of the bug and how it's tested to the comment at the
begining of `test.sh`. You don't need to add the file to git yet.

4. Write the test commands, comment anything that's not obvious.

5. Test your test. Use the `TEST` variable to jump right to your test:
```shell
$ make TEST=012\* tests-misc           # from top directory
$ TEST=012\* ./misc-tests.sh           # from tests/
```

6. The commit changelog should reference a commit that either introduced or
  fixed the bug (or both). Subject line of the shall mention the name of the
  new directory for ease of search, eg. `btrfs-progs: tests: add 012-subvolume-sync-must-wait`
