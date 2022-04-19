#!/bin/bash
#
# Make sure btrfstune will not set seed flag when the fs has dirty log

source "$TEST_TOP/common"

check_prereq btrfstune

image=$(extract_image "./dirty_log.img.xz")

run_mustfail "btrfstune should reject fs with dirty log for seeding flag" \
	"$TOP/btrfstune" -S1 "$image"
