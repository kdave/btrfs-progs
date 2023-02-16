FROM centos:8

WORKDIR /tmp

RUN rpm --import https://www.elrepo.org/RPM-GPG-KEY-elrepo.org
# Errors due to missing mirrors since 01/2022
RUN sed -i -e "s|mirrorlist=|#mirrorlist=|" /etc/yum.repos.d/CentOS-*
RUN sed -i -e "s|#baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|" /etc/yum.repos.d/CentOS-*
RUN yum -y install https://www.elrepo.org/elrepo-release-8.el8.elrepo.noarch.rpm
RUN yum -y install epel-release

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
RUN yum -y install udev device-mapper acl attr xz

# For debugging
RUN yum -y install less vim

COPY ./test-build .
COPY ./run-tests .
COPY ./devel.tar.gz .

CMD ./test-build devel --disable-documentation --disable-zoned

# Continue with:
# cd /tmp
# (see CMD above)
# ./run-tests /tmp/btrfs-progs-devel
