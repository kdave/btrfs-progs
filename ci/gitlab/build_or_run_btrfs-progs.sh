#!/bin/bash
#
# Build or Run btrfs-progs tests.

set -x

BTRFS_BIN="btrfs"
MNT_DIR="/mnt/"
BUILD_DIR="/btrfs/"
test_cmd=$(cat ${MNT_DIR}/cmd)

rm -f ${MNT_DIR}/result
${BTRFS_BIN} --version

if [ $? -ne 0 ]
then
    echo "=========================== Builb btrfs-progs ================"
    echo " Image doesn't have ${BTRFS_BIN} - start build process"
    cd ${MNT_DIR} && ./autogen.sh && ./configure --disable-documentation --disable-backtrace && make -j`nproc` && make install && make testsuite
    echo "================= Prepare Testsuite =========================="
    mkdir -p ${BUILD_DIR}
    cp tests/btrfs-progs-tests.tar.gz ${BUILD_DIR}
    poweroff
else
    echo "================= Run Tests  ================================="
    cd ${BUILD_DIR} && tar -xvf btrfs-progs-tests.tar.gz && ${test_cmd}

    # check test result status
    if [ $? -ne 0 ]; then
       cd ${BUILD_DIR} && cp *tests-results.txt ${MNT_DIR}
       poweroff
    else
       cd ${BUILD_DIR} && cp *tests-results.txt ${MNT_DIR}
       touch ${MNT_DIR}/result
       poweroff
    fi
fi
