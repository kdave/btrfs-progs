#!/bin/bash
#
# Verify that we do not force mixed block groups on small volumes anymore

source "$TEST_TOP/common"

check_prereq mkfs.btrfs

setup_root_helper

mixed=$(run_check_stdout "$TOP/mkfs.btrfs" -b 512M -n 64k -f "$TEST_DEV" | grep -E 'Data|Metadata')
echo "$mixed" | grep -q -v 'Data+Metadata:' || _fail "unexpected: created a mixed-bg filesystem"
