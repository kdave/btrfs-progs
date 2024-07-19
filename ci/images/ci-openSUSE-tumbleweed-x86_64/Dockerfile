FROM opensuse/tumbleweed

WORKDIR /tmp

RUN zypper install -y --no-recommends make gcc tar gzip clang dwarves
RUN zypper install -y --no-recommends libattr-devel libblkid-devel libuuid-devel
RUN zypper install -y --no-recommends libext2fs-devel
RUN zypper install -y --no-recommends zlib-devel lzo-devel libzstd-devel
RUN zypper install -y --no-recommends autoconf automake pkg-config gawk
RUN zypper install -y --no-recommends python3 python3-devel python3-setuptools
RUN zypper install -y --no-recommends libudev-devel

# For downloading fresh sources
RUN zypper install -y --no-recommends wget

# For running tests
RUN zypper install -y --no-recommends coreutils util-linux e2fsprogs findutils grep
RUN zypper install -y --no-recommends udev device-mapper acl attr xz

# For debugging
RUN zypper install -y --no-recommends less vim

# Static build
RUN zypper install -y --no-recommends glibc-devel-static libblkid-devel-static \
	    libcom_err-devel-static libext2fs-devel-static libuuid-devel-static \
	    libzstd-devel-static lzo-devel-static zlib-devel-static

RUN zypper install -y --no-recommends libgcrypt-devel libsodium-devel libkcapi-devel \
	   libbotan-devel libopenssl-3-devel
RUN zypper install -y --no-recommends gcc15

RUN zypper -n --gpg-auto-import-keys ar --no-gpgcheck --refresh https://download.opensuse.org/repositories/devel:/gcc:/next/openSUSE_Tumbleweed/ tw-oss
RUN zypper install -y --allow-vendor-change gcc16
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-16 16

COPY ./test-build .
COPY ./run-tests .
COPY ./devel.tar.gz .

CMD ["./test-build", "devel", "--disable-documentation"]

# Continue with:
# cd /tmp
# (see CMD above)
# ./run-tests /tmp/btrfs-progs-devel
#
# To test static build:
# cd btrfs-progs-devel
# make static
