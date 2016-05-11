# Btrfs-progs tests

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

*tests/misc-tests/:*

  * anything that does not fit to the above, the test driver script will only
    execute `./test.sh` in the test directory

*tests/common:*

  * script with helpers

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
the wrappers (`run_check` etc), other commands are silent.

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

## New test

1. Pick the category for the new test or fallback to `misc-tests` if not sure. For
an easy start copy an existing `test.sh` script from some test that might be
close to the purpose of your new test.

* Use the highest unused number in the sequence, write a short descriptive title
and join by dashes `-`.

* Write a short description of the bug and how it's tested to the comment at the
begining of `test.sh`.

* Write the test commands, comment anything that's not obvious.

* Test your test. Use the `TEST` variable to jump right to your test:
```shell
$ make TEST=012\* tests-misc           # from top directory
$ TEST=012\* ./misc-tests.sh           # from tests/
```

* The commit changelog should reference a commit that either introduced or
  fixed the bug (or both). Subject line of the shall mention the name of the
  new directory for ease of search, eg. `btrfs-progs: tests: add 012-subvolume-sync-must-wait`
