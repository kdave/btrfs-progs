FROM opensuse/tumbleweed

WORKDIR /tmp

RUN zypper install -y --no-recommends autoconf automake pkg-config
RUN zypper install -y --no-recommends libattr-devel libblkid-devel libuuid-devel
RUN zypper install -y --no-recommends libext2fs-devel libreiserfscore-devel
RUN zypper install -y --no-recommends zlib-devel lzo-devel libzstd-devel
RUN zypper install -y --no-recommends make gcc tar gzip
RUN zypper install -y --no-recommends python3 python3-devel python3-setuptools

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

CMD ./test-build devel --disable-documentation

# Continue with:
# cd /tmp
# (see CMD above)
# ./run-tests /tmp/btrfs-progs-devel
