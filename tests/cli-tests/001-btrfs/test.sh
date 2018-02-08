#!/bin/bash
# test commands of btrfs

source "$TEST_TOP/common"

check_prereq btrfs

# returns 1
run_mayfail $TOP/btrfs || true
run_check "$TOP/btrfs" version
run_check "$TOP/btrfs" version --
run_check "$TOP/btrfs" help
run_check "$TOP/btrfs" help --
run_check "$TOP/btrfs" help --full
run_check "$TOP/btrfs" --help
run_check "$TOP/btrfs" --help --full
run_check "$TOP/btrfs" --version
run_check "$TOP/btrfs" --version --help
