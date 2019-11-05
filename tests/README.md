# Btrfs-progs tests

A testsuite covering functionality of btrfs-progs, ie. the checker, image, mkfs
and similar tools. There are no additional requirements on kernel features
(other than `CONFIG_BTRFS_FS` built-in or module), the
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

The tests are prefixed by a number for ordering and uniqueness. To run a
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

will run the first test in fsck-tests subdirectory. If the test directories
follow a good naming scheme, it's possible to select a subset eg. like the
convert tests for ext[234] filesystems.


## Test structure

*tests/fsck-tests/*

  * tests targeted at bugs that are fixable by fsck, the test directory can
    contain images that will get fixed, or a custom script `./test.sh` that
    will be run if present

*tests/convert-tests/*

  * coverage tests of ext2/3/4 or reiserfs and btrfs-convert options

*tests/fuzz-tests/*

  * collection of fuzzed or crafted images
  * tests that are supposed to run various utilities on the images and not
    crash

*tests/cli-tests/*

  * tests for command line interface, option coverage, weird option combinations that should not work
  * not necessary to do any functional testing, could be rather lightweight
  * functional tests should go to to other test dirs
  * the driver script will only execute `./test.sh` in the test directory

*tests/misc-tests/*

  * anything that does not fit to the above, the test driver script will only
    execute `./test.sh` in the test directory

*tests/common, tests/common.convert*

  * scripts with shell helpers, separated by functionality

*tests/test.img*

  * default testing image, available as `TEST_DEV` variable, the file is never
    deleted by the scripts but truncated to 0 bytes, so it keeps it's
    permissions. It's eg. possible to host it on NFS, make it `chmod a+w` for
    root.


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

### Verbosity, test tuning

* `TEST_LOG=tty` -- setting the variable will print all commands executed by
  some of the wrappers (`run_check` etc), other commands are not printed to the
  terminal (but the full output is in the log)

* `TEST_LOG=dump` -- dump the entire testing log when a test fails

* `TEST_ENABLE_OVERRIDE` -- defined either as make arguments or via
  `tests/common.local` to enable additional arguments to some commands, using
  the variable(s) below (default: false, enable by setting to 'true')

* `TEST_ARGS_CHECK` -- user-defined arguments to `btrfs check`, before the
  test-specific arguments

* `TEST_ARGS_MKFS` -- user-defined arguments to `mkfs.btrfs`, before the
  test-specific arguments

* `TEST_ARGS_CONVERT` -- user-defined arguments to `btrfs-convert`, before the
  test-specific arguments

Multiple values can be separated by `,`.

### Permissions

Some commands require root privileges (to mount/umount, access loop devices or
call privileged ioctls).  It is assumed that `sudo` will work in some way (no
password, password asked and cached). Note that instrumentation is not applied
in this case, for safety reasons. You need to modify the test script instead.

### Cleanup

The tests are supposed to cleanup after themselves if they pass. In case of
failure, the rest of the tests are skipped and intermediate files, mounts and
loop devices are kept. This should help to investigate the test failure but at
least the mounts and loop devices need to be cleaned before the next run.

This is partially done by the script `clean-tests.sh`, you may want to check
the loop devices as they are managed on a per-test basis.

### Prototyping tests, quick tests

There's a script `test-console.sh` that will run shell commands in a loop and
logs the output with the testing environment set up. It sources the common
helper scripts so the shell functions are available.

### Runtime dependencies

The tests use some common system utilities like `find`, `rm`, `dd`. Additionally,
specific tests need the following packages installed: `acl`, `attr`,
`e2fsprogs`, `reiserfsprogs`.


## New test

1. Pick the category for the new test or fallback to `misc-tests` if not sure. For
an easy start copy an existing `test.sh` script from some test that might be
close to the purpose of your new test. The environment setup includes the
common scripts and/or prepares the test devices. Other scripts contain examples
how to do mkfs, mount, unmount, check, loop device management etc.

2. Use the highest unused number in the sequence, write a short descriptive title
and join by dashes `-`. This will become the directory name, eg. `012-subvolume-sync-must-wait`.

3. Write a short description of the bug and how it's tested to the comment at the
beginning of `test.sh`. You don't need to add the file to git yet. Don't forget
to make the file executable, otherwise it's not going to be executed by the
infrastructure.

4. Write the test commands, comment anything that's not obvious.

5. **Test your test.** Use the `TEST` variable to jump right to your test:
```shell
$ make TEST=012\* tests-misc           # from top directory
$ TEST=012\* ./misc-tests.sh           # from tests/
```

6. The commit changelog should reference a commit that either introduced or
  fixed the bug (or both). Subject line of the shall mention the name of the
  new directory for ease of search, eg. `btrfs-progs: tests: add 012-subvolume-sync-must-wait`

7. A commit that fixes a bug should be applied before the test that verifies
  the fix. This is to keep the git history bisectable.


### Test images

Most tests should be able to create the test images from scratch, using regular
commands and file operation. The commands also document the testcase and use
the test code and kernel of the environment.

In other cases, a pre-created image may be the right way if the above does not
work (eg. comparing output, requesting an exact layout or some intermediate
state that would be hard to achieve otherwise).

* images that don't need data and valid checksums can be created by
  `btrfs-image`, the image can be compressed by the tool itself (file extension
  `.img`) or compressed externally (recognized is `.img.xz`)

* raw images that are binary dump of an existing image, created eg. from a
  sparse file (`.raw` or `.raw.xz`)

Use `xz --best` and try to get the smallest size as the file is stored in git.


### Crafted/fuzzed images

Images that are created by fuzzing or specially crafted to trigger some error
conditions should be added to the directory *fuzz-tests/images*, accompanied by
a textual description of the source (bugzilla, mail), the reporter, brief
description of the problem or the stack trace.

If you have a fix for the problem, please submit it prior to the test image, so
the fuzz tests always succeed when run on random checked out. This helps
bisectability.


# Exported testsuite

The tests are typically run from git on binaries built from the git sources. It
is possible to extract only the testsuite files and run it independently. Use

```shell
$ make testsuite
```

This will gather scripts and generate `tests/btrfs-progs-tests.tar.gz`. The
files inside the tar are in the top level directory, make sure you extract
the contents to an empty directory. From there you can start the tests as
described above (the non-make variant).

By default the binaries found in `$PATH` are used, this will normally mean the
system binaries. You can also override the `$TOP` shell variable and this
path will be used as prefix for all btrfs binaries inside the tests.

There are some utilities that are not distributed but are necessary for the
tests. They are in the top level directory of the testsuite and their path
cannot be set.

The tests assume write access to their directories.


# Coding style, best practices

## do

* quote all variables by default, any path, even the TOP could need that, and
  we use it everywhere
  * even if the variable is safe, use quotes for consistency and to ease
    reading the code
  * there are exceptions:
    * `$SUDO_HELPER` as it might be intentionally unset
* use `#!/bin/bash` explicitly
* check for all external dependencies (`check_prereq_global`)
* check for internal dependencies (`check_prereq`), though the basic set is
  always built when the tests are started through make
* use functions instead of repeating code
  * generic helpers could be factored to the `common` script
* cleanup files an intermediate state (mount, loop devices, device mapper
  devices) a after successful test
* use common helpers and variables where possible

## do not

* pull external dependencies if we can find a way to replace them: example is
  `xfs_io` that's conveniently used in fstests but we'd require `xfsprogs`,
  so use `dd` instead
* throw away (redirect to */dev/null*) output of commands unless it's justified
  (ie. really too much text, unnecessary slowdown) -- the test output log is
  regenerated all the time and we need to be able to analyze test failures or
  just observe how the tests progress
* cleanup after failed test -- the testsuite stops on first failure and the
  developer can eg. access the environment that the test created and do further
  debugging
  * this might change in the future so the tests cover as much as possible, but
    this would require to enhance all tests with a cleanup phase
