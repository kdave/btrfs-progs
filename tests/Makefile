#
# Testing-specific options (see also README.md):
#   TEST=GLOB      run test(s) from directories matching GLOB
#   TEST_LOG=tty   print name of a command run via the execution helpers
#   TEST_LOG=dump  dump testing log file when a test fails
#   TEST_FLAVOR    test build flavor: dynamic (default), static
#
#   TOP=path       test binaries from the given path

# Export all variables to sub-makes by default
export

ABSTOPDIR = $(shell pwd)
TOPDIR := .

ifeq ("$(origin V)", "command line")
  BUILD_VERBOSE = $(V)
endif
ifndef BUILD_VERBOSE
  BUILD_VERBOSE = 0
endif

ifeq ($(BUILD_VERBOSE),1)
  Q =
else
  Q = @
endif

MAKEOPTS = --no-print-directory Q=$(Q)

.PHONY: all
.PHONY: FORCE

test: test-check test-check-lowmem test-mkfs test-misc test-cli test-convert test-fuzz

test-convert:
	@echo "    [TEST]   convert-tests.sh"
	$(Q)bash ./convert-tests.sh

test-check: test-fsck
test-check-lowmem: test-fsck
test-fsck:
ifneq ($(MAKECMDGOALS),test-check-lowmem)
	@echo "    [TEST]   fsck-tests.sh"
	$(Q)bash ./fsck-tests.sh
else
	@echo "    [TEST]   fsck-tests.sh (mode=lowmem)"
	$(Q)TEST_ENABLE_OVERRIDE=true TEST_ARGS_CHECK=--mode=lowmem bash ./fsck-tests.sh
endif

test-misc:
	@echo "    [TEST]   misc-tests.sh"
	$(Q)bash ./misc-tests.sh

test-mkfs:
	@echo "    [TEST]   mkfs-tests.sh"
	$(Q)bash ./mkfs-tests.sh

test-fuzz:
	@echo "    [TEST]   fuzz-tests.sh"
	$(Q)bash ./fuzz-tests.sh

test-cli:
	@echo "    [TEST]   cli-tests.sh"
	$(Q)bash ./cli-tests.sh

test-clean:
	@echo "Cleaning tests"
	$(Q)bash ./clean-tests.sh
