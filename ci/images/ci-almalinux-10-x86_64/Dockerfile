FROM almalinux:10

WORKDIR /tmp

#RUN rpm --import https://www.elrepo.org/RPM-GPG-KEY-elrepo.org
#RUN yum -y install https://www.elrepo.org/elrepo-release-10.el10.elrepo.noarch.rpm
#RUN yum -y install epel-release

RUN yum -y install autoconf automake pkg-config
RUN yum -y install libattr-devel libblkid-devel libuuid-devel
RUN yum -y install e2fsprogs-libs e2fsprogs-devel
RUN yum -y install zlib-devel lzo-devel libzstd-devel zstd
RUN yum -y install make gcc tar gzip clang
RUN yum -y install python3 python3-devel python3-setuptools

# For downloading fresh sources
RUN yum -y install wget

# For running tests
RUN yum -y install util-linux e2fsprogs findutils grep
RUN yum -y install udev systemd-devel device-mapper acl attr xz

RUN yum -y install libsodium-devel

# For debugging
RUN yum -y install less vim

COPY ./test-build .
COPY ./run-tests .
COPY ./devel.tar.gz .

CMD ["./test-build", "devel", "--disable-documentation"]

# Continue with:
# cd /tmp
# (see CMD above)
# ./run-tests /tmp/btrfs-progs-devel
