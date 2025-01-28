#!/bin/bash
# test commands of btrfs

source "$TEST_TOP/common" || exit

check_prereq btrfs

# returns 1
run_mayfail "$TOP/btrfs" || true
run_check "$TOP/btrfs" version
run_check "$TOP/btrfs" version --
run_check "$TOP/btrfs" help
run_check "$TOP/btrfs" help --
run_check "$TOP/btrfs" help --full
run_check "$TOP/btrfs" --help
run_check "$TOP/btrfs" --help --full
run_check "$TOP/btrfs" --version
run_check "$TOP/btrfs" --version --help

# Log levels
run_check "$TOP/btrfs" --log=default help
run_check "$TOP/btrfs" --log=info help
run_check "$TOP/btrfs" -vv help
run_check "$TOP/btrfs" --log=verbose help
run_check "$TOP/btrfs" -vvv help
run_check "$TOP/btrfs" --log=debug help
run_check "$TOP/btrfs" -vvvv help
run_check "$TOP/btrfs" --log=quiet help
run_check "$TOP/btrfs" -q help
run_mustfail "invalid log level accepted" "$TOP/btrfs" --log=invalid help

# Combine help with other options
run_mustfail "unrecognized option accepted" "$TOP/btrfs" filesystem df -v
run_mustfail "unrecognized option accepted" "$TOP/btrfs" filesystem df -v /
run_mustfail "unrecognized option accepted" "$TOP/btrfs" filesystem df -v --help /
if ! run_check_stdout "$TOP/btrfs" filesystem df --help / | grep -q 'usage.*filesystem df'; then
	_fail "standalone option --help"
fi
if ! run_check_stdout "$TOP/btrfs" filesystem df -H --help / | grep -q 'usage.*filesystem df'; then
	_fail "option --help with valid option (1)"
fi
if ! run_check_stdout "$TOP/btrfs" filesystem df --help -H / | grep -q 'usage.*filesystem df'; then
	_fail "option --help with valid option (2)"
fi
