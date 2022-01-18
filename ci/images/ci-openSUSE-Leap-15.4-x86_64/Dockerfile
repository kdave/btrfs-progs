FROM opensuse/leap:15.4

WORKDIR /tmp

RUN zypper install -y --no-recommends autoconf automake pkg-config
RUN zypper install -y --no-recommends libattr-devel libblkid-devel libuuid-devel
RUN zypper install -y --no-recommends libext2fs-devel libreiserfscore-devel
RUN zypper install -y --no-recommends zlib-devel lzo-devel libzstd-devel
RUN zypper install -y --no-recommends make gcc tar gzip clang
RUN zypper install -y --no-recommends python3 python3-devel python3-setuptools
RUN zypper install -y --no-recommends libudev-devel

# For downloading fresh sources
RUN zypper install -y --no-recommends wget

# For running tests
RUN zypper install -y --no-recommends coreutils util-linux e2fsprogs findutils grep
RUN zypper install -y --no-recommends udev device-mapper acl attr xz

# For debugging
RUN zypper install -y --no-recommends less vim

COPY ./test-build .
COPY ./run-tests .
COPY ./devel.tar.gz .

# The blkzoned.h exists but blk_zone.capacity is missing, disable zoned mode explicitly
CMD ./test-build devel --disable-documentation --disable-zoned

# Continue with:
# cd /tmp
# (see CMD above)
# ./run-tests /tmp/btrfs-progs-devel
