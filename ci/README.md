# CI

Continuous integration, testing inside containers. Most of the *btrfs-progs*
functionality is in user space and does not need a virtual machine. The
features supported by the running kernel are detected and tests skipped
eventually.

## Hosted

**Travis CI**

The Travis service is set up to run tests on development and release branches,
triggered by a push to the repository. Pull requests are not set up to run.

[Branch overview](https://travis-ci.org/kdave/btrfs-progs)

**Gitlab**

The integration with gitlab.org has been disabled but is possible to revive. We
were experimenting with nested virtualization to run the tests on a current
kernel not some old version provided by the hosted image. The tests took to
long to fit in the free plan quota.

## Local

The testsuite can be run directly from the git tree from the built sources, or
from the separate exported testsuite. This depends on the installed system
packages and running kernel.

Another option is to run the tests on a given distribution in a container.
There are several *docker* container images for some distributions. Right now
they're meant for testing development branch *devel*, but can be adapted for
others as well.

**Build tests**

The simplest test is to verify that the project builds on a given distribution.
The backward compatibility of *btrfs-progs* is supposed to cover also old and
long-term support distributions, as well as systems with standard C library
other than GNU glibc. Some features like run-time stack trace dump are not
available but can be disabled at configure time.

**Functional tests**

By default only the build test is run in the container. There's a script to
start the testsuite, although this can be also done manually by running the
appropriate commands (check the script *ci/images/run-tests*)

**Fine-tuned tests**

The build supports additional features like sanitizers, enabled by environment
variables. These can be passed to the container environment, see examples below.

**The container environment**

The tests need to run privileged (to create loop devices and mount/unmount
filesystems) and need to see the block devices (created by device mapper).
Starting the container as *docker run* might not be sufficient without
parameters and additional mounts.

To minimize the image size and installation dependencies, the documentation is
not built by default and lacks the tools to build it, so you need to pass
*--disable-documentation* for the builder scripts or for the raw *configure*
command.

### Examples

Assuming top level directory in the *btrfs-progs* git repository, then moved
to directory with a particular image sources.

**Prepare image**

    cd ci/images/ci-openSUSE-tumbleweed-x86_64
    ./docker-build

Running plain *docker build* may not work as some magic is needed to allow
building either the branch from web repository, or from a local git branch
provided as a tarball. Docker does not allow conditional image contents so this
is pushed to the test build scripts.

**Build**

Neither running the image is just *docker run*, so there's a script for
convenience:

    ./docker-run

You can pass additional docker parameters or a non-default command:

    ./docker-run --env=VAR=text

or

    ./docker-run --env=V=1 -- ./test-build devel --disable-documentation

The *--* is separator for docker and the actual command. The command above will
effectively run the make command with *V=1* ie. raw commands as they're
executed. Other options work as well, see the top level Makefile. Notably, the
sanitizers can be enabled like

    ./docker-run --env=D=asan -- ./test-build devel --disable-documentation

This will just build the sources.

**Build and run tests**

In order to run the whole testsuite one more script needs to be run:

    ./docker-run --env=D=asan -- bash -c "./test-build devel --disable-documentation && \
            ./run-tests /tmp/btrfs-progs-devel"

As docker does not allow to run multiple commands, you can either start the
whole command wrapped in a shell or use the script
*ci/images/docker-run-tests*.

## What else

The current set of build targets covers commonly used systems, in terms of
package versions. There are no significant differences between many
distributions and adding support for each does not bring any benefits. If you
think there's something that can improve build coverage and catch errors during
development, please open an issue or send a pull request adding a new docker
image template or enhancing current support scripts.

To do:

- 32bit coverage -- while this architecture is fading out, it may be useful to
  still have some coverage, however running 32bit docker in 64bit is not
  considered experimental does not work out of the box
- static build -- when all build dependencies provide the static library
  versions, it's possible to to build the static binaries of all the tools
- add some kind of templates, there's a lot of repeated stuff in the
  *Dockerfile*s and the scripts need to be inside the directories in order to
  allow copying them to the image
