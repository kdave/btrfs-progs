#!/bin/bash
#
# Verify that we do not force mixed block groups on small volumes anymore

source $TOP/tests/common

check_prereq mkfs.btrfs

setup_root_helper

run_check truncate -s 512M $IMAGE
mixed=$(run_check_stdout $TOP/mkfs.btrfs -n 64k -f $IMAGE | egrep 'Data|Metadata')
echo "$mixed" | grep -q -v 'Data+Metadata:' || _fail "unexpected: created a mixed-bg filesystem"
