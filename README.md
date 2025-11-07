Btrfs-progs
===========

[![devel](https://github.com/kdave/btrfs-progs/actions/workflows/devel.yml/badge.svg)](https://github.com/kdave/btrfs-progs/actions/workflows/devel.yml)
[![coverage](https://github.com/kdave/btrfs-progs/actions/workflows/coverage.yml/badge.svg)](https://github.com/kdave/btrfs-progs/actions/workflows/coverage.yml)
[![codecov](https://codecov.io/gh/kdave/btrfs-progs/branch/coverage-test/graph/badge.svg?token=fhLI8V9s0k)](https://codecov.io/gh/kdave/btrfs-progs)
[![static](https://github.com/kdave/btrfs-progs/actions/workflows/artifacts-static-build.yml/badge.svg)](https://github.com/kdave/btrfs-progs/actions/workflows/artifacts-static-build.yml)
[![release](https://github.com/kdave/btrfs-progs/actions/workflows/image-build-test.yml/badge.svg)](https://github.com/kdave/btrfs-progs/actions/workflows/image-build-test.yml)

Userspace utilities to manage btrfs filesystems.
License: GPLv2.

Btrfs is a copy on write (COW) filesystem for Linux aimed at implementing
advanced features while focusing on fault tolerance, repair and easy
administration.

This repository hosts following utilities and also documentation:

* **btrfs** &mdash; the main administration tool ([manual page](https://btrfs.readthedocs.io/en/latest/btrfs.html))
* **mkfs.btrfs** &mdash; utility to create the filesystem ([manual page](https://btrfs.readthedocs.io/en/latest/mkfs.btrfs.html))
* all-in-one binary in the busybox style with mkfs.btrfs, btrfs-image and other tools built-in ([standalone tools](https://btrfs.readthedocs.io/en/latest/btrfs.html#standalone-tools))
* **libbtrfsutil** (LGPL v2.1) &mdash; C and python 3 bindings, see [libbtrfsutil/README.md](libbtrfsutil/README.md) for more
* **btrfsutil** python bindings published at https://pypi.org/project/btrfsutil
* manual pages and documentation source published at [btrfs.readthedocs.io](https://btrfs.readthedocs.io) (RTD)

See [INSTALL](INSTALL) for build instructions, [tests/README.md](tests/README.md) for
testing information and [ci/README.md](ci/README.md) for CI information.

Release cycle
-------------

The major version releases are time-based and follow the cycle of the linux
kernel releases. The cycle usually takes 2 months. A minor version release may
happen in the meantime if there are bug fixes or minor useful improvements
queued.

The release tags are signed with a GPG key ID `F2B4 1200 C54E FB30 380C  1756 C565 D5F9 D76D 583B`,
release tarballs are hosted at [kernel.org](https://www.kernel.org/pub/linux/kernel/people/kdave/btrfs-progs/).
See file [CHANGES](CHANGES) or [changelogs on RTD](https://btrfs.readthedocs.io/en/latest/CHANGES.html).

Releases with changelog are also published at [Github release page](https://github.com/kdave/btrfs-progs/releases).

### Static binaries

For each release there are static binaries of `btrfs` and `btrfs.box` provided.
These can be used in rescue environments and are built for `x86_64`
architecture (with maximum backward compatibility), inside the [Github Actions
workflow](https://github.com/kdave/btrfs-progs/actions/workflows/artifacts-static-build.yml).
The `btrfs.box` is an all-in-one tool in the [busybox](https://www.busybox.net)
style, the functionality is determined by the binary names (either symlink,
hardlink or a file copy).

### Feature compatibility

The *btrfs-progs* of version *X.Y* declare support of kernel features of the same
version. New progs on old kernel are expected to work, limited only by features
provided by the kernel.

### Build compatibility

Build is supported on the [GNU C library](https://www.gnu.org/software/libc/)
as the primary target, and on the [musl libc](https://musl.libc.org/)
and [uClibc-ng](https://www.uclibc-ng.org/).

The supported compilers are [gcc](https://gcc.gnu.org/) (minimal version 4.8)
and [clang](https://clang.llvm.org/) (minimal version 3.4).

Build tests are done on [several distributions](https://github.com/kdave/btrfs-progs/blob/master/.github/workflows/image-build-test.yml), see
[Github actions workflow](https://github.com/kdave/btrfs-progs/actions/workflows/image-build-test.yml).

Reporting bugs
--------------

There are several ways, each has its own specifics and audience that can give
feedback or work on a fix. The following list is sorted in the order of
preference:

* [Github issue tracker](https://github.com/kdave/btrfs-progs/issues)
* to the mailing list *linux-btrfs@vger.kernel.org* -- (not required to
  subscribe), beware that the mail might get overlooked in other traffic
* IRC (irc.libera.chat #btrfs) -- good for discussions eg. if a bug is already
  known, but reports could miss developers' attention
* please don't use https://bugzilla.kernel.org for btrfs-progs bugs

Development
-----------

The development takes place in the mailing list (*linux-btrfs@vger.kernel.org*)
or at Github (issues, pull requests). Changes should be split to logical parts
if possible, documentation may be included in the same patch as to code or
separately.

The development model of btrfs-progs shares a lot with the kernel model. The

* **one logical change per patch**: e.g. not mixing bugfixes, cleanups, features
  etc., sometimes it's not clear and will be usually pointed out during reviews
* proper **subject line**: e.g. prefix with _btrfs-progs: subpart, ..._ ,
  descriptive yet not too long, see `git log --oneline` for some inspiration
* proper **changelog**: the changelogs are often missing or lacking explanation _why_
  the change was made, or _how_ is something broken, _what_ are user-visible
  effects of the bug or the fix, _how_ does an improvement help or the intended
  _usecase_
* the **Signed-off-by** line is not mandatory for less significant changes
  (typos, documentation) but is desired as this documents who authored the
  change, you can read more about the
  [The Developer's Certificate of Origin (chapter 11)](https://www.kernel.org/doc/html/latest/process/submitting-patches.html#sign-your-work-the-developer-s-certificate-of-origin)
  * if you are not used to the signed-off-by style, your contributions won't be
    rejected just because it's missing, some other way of attribution will be
    used, like a link to issue or pull request to allow contributions without
    much bothering with formalities
  * project maintainers will add their Signed-off-by tag regardless

### Pull requests

The pull requests on Github may be used for code or documentation
contributions. There are basic build checks enabled in the Github actions CI
for pull requests. The status can be checked at the
[workflow page](https://github.com/kdave/btrfs-progs/actions/workflows/pull-request.yml).

* open a PR against branches *devel* or *master*
* push update to the same branch if you need to
* close the PR in case it's wrong, a mistake or needs rework
* if you're sure the changes don't need a CI build verification, please add `[skip ci]` to the changelog

Source code coding style and preferences follow the
[kernel coding style](https://www.kernel.org/doc/html/latest/process/coding-style.html).
You can find the editor settings in `.editorconfig` and use the
[EditorConfig](https://editorconfig.org/) plugin to let your editor use that,
or update your editor settings manually.

Testing
-------

The documentation for writing and running tests can be found in
[tests/](tests/README.md) and continuous integration/container images in
[ci/](ci/README.md).

Development branches are tested by Github
[Action workflows](https://github.com/kdave/btrfs-progs/actions).

Code coverage provided by [codecov.io](https://codecov.io) can be found
[here](https://codecov.io/gh/kdave/btrfs-progs).

Documentation updates
---------------------

Documentation fixes or updates do not need much explanation so sticking to the
code rules in the previous section is not necessary. GitHub pull requests are
OK, patches could be sent to me directly and not required to be also in the
mailing list. Pointing out typos via IRC also works, although might get
accidentally lost in the noise.

Documentation sources are written in
[RST](https://en.wikipedia.org/wiki/ReStructuredText) and built by sphinx.

Third-party sources
-------------------

Build dependencies are listed in [INSTALL](INSTALL). Implementation of checksum/hash
functions is provided by copies of the respective sources to avoid adding
dependencies that would make deployments in rescue or limited environments
harder. The implementations are portable and there are optimized versions for
some architectures.  Optionally it's possible to use
[libgcrypt](https://www.gnupg.org/software/libgcrypt/index.html),
[libsodium](https://doc.libsodium.org),
[libkcapi](https://www.chronox.de/libkcapi.html),
[Botan](https://botan.randombit.net) or
[OpenSSL](https://www.openssl.org) implementations.

The builtin implementations uses the following sources:
[CRC32C](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git),
[XXHASH](https://github.com/Cyan4973/xxHash),
[SHA256](https://tools.ietf.org/html/rfc4634),
[BLAKE2](https://github.com/BLAKE2/BLAKE2).

Some other code is borrowed from kernel, eg. the raid5 tables or data structure
implementation (list, rb-tree).

References
----------

* [Documentation](https://btrfs.readthedocs.io)
* [Manual pages](https://btrfs.readthedocs.io/en/latest/man-index.html)
* [Changes -- btrfs-progs](https://btrfs.readthedocs.io/en/latest/CHANGES.html)
* [Features by kernel version](https://btrfs.readthedocs.io/en/latest/Feature-by-version.html)
