#
# Basic build targets:
#   all		all main tools and the shared library
#   static      build static binaries, requires static version of the libraries
#   test        run the full testsuite
#   install     install binaries, shared libraries and header files to default
#               location (/usr/local)
#   install-static
#               install the static binaries, static libraries and header files
#               to default locationh (/usr/local)
#   clean       clean built binaries (not the documentation)
#   clean-all   clean as above, clean docs and generated files
#   clean-dep   clean header dependency files (*.o.d)
#
# All-in-one binary (busybox style):
#   btrfs.box         single binary with functionality of mkfs.btrfs, btrfs-image,
#                     btrfs-convert and btrfstune, selected by the file name
#   btrfs.box.static  dtto, static version
#
# Tuning by variables (environment or make arguments):
#   V=1            verbose, print command lines (default: quiet)
#   C=1            run checker before compilation (default checker: sparse)
#   D=1            debugging build, turn off optimizations
#   D=dflags       dtto, turn on additional debugging features:
#                  verbose - print file:line along with error/warning messages
#                  trace   - print trace before the error/warning messages
#                  abort   - call abort() on first error (dumps core)
#                  all     - shortcut for all of the above
#                  asan    - enable address sanitizer compiler feature
#                  tsan    - enable thread sanitizer compiler feature
#                  ubsan   - undefined behaviour sanitizer compiler feature
#                  bcheck  - extended build checks
#                  gcov    - enable GCOV support during build
#   W=123          build with warnings (default: off)
#   DEBUG_CFLAGS   additional compiler flags for debugging build
#   EXTRA_CFLAGS   additional compiler flags
#   EXTRA_LDFLAGS  additional linker flags
#   EXTRA_PYTHON_CFLAGS   additional compiler flags to pass when building Python
#                         library
#   EXTRA_PYTHON_LDFLAGS  additional linker flags to pass when building Python
#                         library
#
# Testing-specific options (see also tests/README.md):
#   TEST=GLOB      run test(s) from directories matching GLOB
#   TEST_LOG=tty   print name of a command run via the execution helpers
#   TEST_LOG=dump  dump testing log file when a test fails
#
# Static checkers:
#   CHECKER        static checker binary to be called (default: sparse)
#   CHECKER_FLAGS  flags to pass to CHECKER, can override CFLAGS
#

# Export all variables to sub-makes by default
export

-include Makefile.inc
ifneq ($(MAKEFILE_INC_INCLUDED),yes)
$(error Makefile.inc not generated, please configure first)
endif

TAGS_CMD := ctags
ETAGS_CMD := etags
CSCOPE_CMD := cscope -u -b -c -q

include Makefile.extrawarn

EXTRA_CFLAGS :=
EXTRA_LDFLAGS :=

DEBUG_CFLAGS_DEFAULT = -O0 -U_FORTIFY_SOURCE -ggdb3 -DINJECT
DEBUG_CFLAGS_INTERNAL =
DEBUG_CFLAGS :=

DEBUG_LDFLAGS_DEFAULT =
DEBUG_LDFLAGS_INTERNAL =
DEBUG_LDFLAGS :=

ABSTOPDIR = $(shell pwd)
TOPDIR := .

# Disable certain GCC 8 + glibc 2.28 warning for snprintf()
# where string truncation for snprintf() is expected.
# For GCC9 disable address-of-packed (under W=1)
DISABLE_WARNING_FLAGS := $(call cc-disable-warning, format-truncation) \
	$(call cc-disable-warning, address-of-packed-member)

# Warnings that we want by default
ENABLE_WARNING_FLAGS := $(call cc-option, -Wimplicit-fallthrough) \
			$(call cc-option, -Wmissing-prototypes)

ASFLAGS =

# Common build flags
CFLAGS = $(SUBST_CFLAGS) \
	 -std=gnu11 \
	 -include include/config.h \
	 -DBTRFS_FLAT_INCLUDES \
	 -D_XOPEN_SOURCE=700  \
	 -fno-strict-aliasing \
	 -fPIC \
	 -Wall \
	 -Wunused-but-set-parameter \
	 -I$(TOPDIR) \
	 -I$(TOPDIR)/include \
	 $(CRYPTO_CFLAGS) \
	 -DCOMPRESSION_LZO=$(COMPRESSION_LZO) \
	 -DCOMPRESSION_ZSTD=$(COMPRESSION_ZSTD) \
	 $(DISABLE_WARNING_FLAGS) \
	 $(ENABLE_WARNING_FLAGS) \
	 $(EXTRAWARN_CFLAGS) \
	 $(DEBUG_CFLAGS_INTERNAL) \
	 $(EXTRA_CFLAGS)

LIBBTRFSUTIL_CFLAGS = $(SUBST_CFLAGS) \
		      -std=gnu11 \
		      -D_GNU_SOURCE \
		      -fPIC \
		      -fvisibility=hidden \
		      -I$(TOPDIR)/libbtrfsutil \
		      $(EXTRAWARN_CFLAGS) \
		      $(DEBUG_CFLAGS_INTERNAL) \
		      $(EXTRA_CFLAGS)

LDFLAGS = $(SUBST_LDFLAGS) \
	  -rdynamic -L$(TOPDIR) \
	  $(DEBUG_LDFLAGS_INTERNAL) \
	  $(EXTRA_LDFLAGS)

LIBBTRFSUTIL_LDFLAGS = $(SUBST_LDFLAGS) \
		       -rdynamic -L$(TOPDIR) \
		       $(DEBUG_LDFLAGS_INTERNAL) \
		       $(EXTRA_LDFLAGS)

# Default implementation
CRYPTO_OBJECTS =

ifeq ($(HAVE_CFLAG_msse2),1)
crypto_blake2b_sse2_cflags = -msse2
endif
ifeq ($(HAVE_CFLAG_msse41),1)
crypto_blake2b_sse41_cflags = -msse4.1
endif
ifeq ($(HAVE_CFLAG_mavx2),1)
crypto_blake2b_avx2_cflags = -mavx2
endif
ifeq ($(HAVE_CFLAG_msha),1)
crypto_sha256_x86_cflags = -msse4.1 -msha
endif

LIBS = $(LIBS_BASE) $(LIBS_CRYPTO)
LIBBTRFS_LIBS = $(LIBS_BASE) $(LIBS_CRYPTO)

# Static compilation flags
STATIC_CFLAGS = $(CFLAGS) -ffunction-sections -fdata-sections -DSTATIC_BUILD
STATIC_LDFLAGS = $(SUBST_LDFLAGS) $(EXTRA_LDFLAGS) -static -Wl,--gc-sections
STATIC_LIBS = $(STATIC_LIBS_BASE)

# don't use FORTIFY with sparse because glibc with FORTIFY can
# generate so many sparse errors that sparse stops parsing,
# which masks real errors that we want to see.
# Note: additional flags might get added per-target later
CHECKER := sparse
check_defs := .cc-defines.h
CHECKER_FLAGS := -include $(check_defs) -D__CHECKER__ \
	-D__CHECK_ENDIAN__ -Wbitwise -Wuninitialized -Wshadow -Wundef \
	-U_FORTIFY_SOURCE -Wdeclaration-after-statement -Wdefault-bitfield-sign

objects = \
	kernel-lib/list_sort.o	\
	kernel-lib/raid56.o	\
	kernel-lib/rbtree.o	\
	kernel-lib/tables.o	\
	kernel-shared/accessors.o	\
	kernel-shared/async-thread.o	\
	kernel-shared/backref.o \
	kernel-shared/ctree.o	\
	kernel-shared/delayed-ref.o	\
	kernel-shared/dir-item.o	\
	kernel-shared/disk-io.o	\
	kernel-shared/extent-io-tree.o	\
	kernel-shared/extent-tree.o	\
	kernel-shared/extent_io.o	\
	kernel-shared/file-item.o	\
	kernel-shared/file.o	\
	kernel-shared/free-space-cache.o	\
	kernel-shared/free-space-tree.o	\
	kernel-shared/inode-item.o	\
	kernel-shared/inode.o	\
	kernel-shared/locking.o	\
	kernel-shared/messages.o	\
	kernel-shared/print-tree.o	\
	kernel-shared/root-tree.o	\
	kernel-shared/transaction.o	\
	kernel-shared/tree-checker.o	\
	kernel-shared/ulist.o	\
	kernel-shared/uuid-tree.o	\
	kernel-shared/volumes.o	\
	kernel-shared/zoned.o	\
	common/array.o		\
	common/cpu-utils.o	\
	common/device-scan.o	\
	common/device-utils.o	\
	common/extent-cache.o	\
	common/extent-tree-utils.o	\
	common/filesystem-utils.o	\
	common/format-output.o	\
	common/fsfeatures.o	\
	common/help.o	\
	common/inject-error.o	\
	common/messages.o	\
	common/open-utils.o	\
	common/parse-utils.o	\
	common/path-utils.o	\
	common/rbtree-utils.o	\
	common/send-stream.o	\
	common/send-utils.o	\
	common/sort-utils.o	\
	common/string-table.o	\
	common/string-utils.o	\
	common/sysfs-utils.o	\
	common/task-utils.o \
	common/units.o	\
	common/utils.o	\
	check/qgroup-verify.o	\
	check/repair.o	\
	cmds/receive-dump.o	\
	crypto/crc32c.o	\
	crypto/hash.o	\
	crypto/xxhash.o	\
	$(CRYPTO_OBJECTS)	\
	libbtrfsutil/stubs.o	\
	libbtrfsutil/subvolume.o

cmds_objects = cmds/subvolume.o cmds/subvolume-list.o \
	       cmds/filesystem.o cmds/device.o cmds/scrub.o \
	       cmds/inspect.o cmds/balance.o cmds/send.o cmds/receive.o \
	       cmds/quota.o cmds/qgroup.o cmds/replace.o check/main.o \
	       cmds/restore.o cmds/rescue.o cmds/rescue-chunk-recover.o \
	       cmds/rescue-super-recover.o \
	       cmds/property.o cmds/filesystem-usage.o cmds/inspect-dump-tree.o \
	       cmds/inspect-dump-super.o cmds/inspect-tree-stats.o cmds/filesystem-du.o \
	       cmds/reflink.o \
	       mkfs/common.o check/mode-common.o check/mode-lowmem.o \
	       check/clear-cache.o

libbtrfs_objects = \
		kernel-lib/rbtree.o	\
		libbtrfs/send-stream.o	\
		libbtrfs/send-utils.o	\
		libbtrfs/crc32c.o

libbtrfs_headers = libbtrfs/send-stream.h libbtrfs/send-utils.h libbtrfs/send.h kernel-lib/rbtree.h \
	       kernel-lib/list.h kernel-lib/rbtree_types.h libbtrfs/kerncompat.h \
	       libbtrfs/ioctl.h libbtrfs/ctree.h libbtrfs/version.h
libbtrfsutil_major := $(shell sed -rn 's/^\#define BTRFS_UTIL_VERSION_MAJOR ([0-9])+$$/\1/p' libbtrfsutil/btrfsutil.h)
libbtrfsutil_minor := $(shell sed -rn 's/^\#define BTRFS_UTIL_VERSION_MINOR ([0-9])+$$/\1/p' libbtrfsutil/btrfsutil.h)
libbtrfsutil_patch := $(shell sed -rn 's/^\#define BTRFS_UTIL_VERSION_PATCH ([0-9])+$$/\1/p' libbtrfsutil/btrfsutil.h)
libbtrfsutil_version := $(libbtrfsutil_major).$(libbtrfsutil_minor).$(libbtrfsutil_patch)
libbtrfsutil_objects = libbtrfsutil/errors.o libbtrfsutil/filesystem.o \
		       libbtrfsutil/subvolume.o libbtrfsutil/qgroup.o \
		       libbtrfsutil/stubs.o
convert_objects = convert/main.o convert/common.o convert/source-fs.o \
		  convert/source-ext2.o convert/source-reiserfs.o \
		  mkfs/common.o check/clear-cache.o
mkfs_objects = mkfs/main.o mkfs/common.o mkfs/rootdir.o
image_objects = image/main.o image/sanitize.o image/image-create.o image/common.o \
		image/image-restore.o
tune_objects = tune/main.o tune/seeding.o tune/change-uuid.o tune/change-metadata-uuid.o \
	       tune/convert-bgt.o tune/change-csum.o check/clear-cache.o
all_objects = $(objects) $(cmds_objects) $(libbtrfs_objects) $(convert_objects) \
	      $(mkfs_objects) $(image_objects) $(tune_objects) $(libbtrfsutil_objects)

udev_rules = 64-btrfs-dm.rules 64-btrfs-zoned.rules

ifeq ("$(origin V)", "command line")
  BUILD_VERBOSE = $(V)
endif
ifndef BUILD_VERBOSE
  BUILD_VERBOSE = 0
endif

ifeq ($(BUILD_VERBOSE),1)
  Q =
  SETUP_PY_Q =
else
  Q = @
  SETUP_PY_Q = -q
endif

ifeq ("$(origin D)", "command line")
  DEBUG_CFLAGS_INTERNAL = $(DEBUG_CFLAGS_DEFAULT) $(DEBUG_CFLAGS)
  DEBUG_LDFLAGS_INTERNAL = $(DEBUG_LDFLAGS_DEFAULT) $(DEBUG_LDFLAGS)
endif

ifneq (,$(findstring gcov,$(D)))
  DEBUG_CFLAGS_INTERNAL += -fprofile-arcs -ftest-coverage --coverage
  DEBUG_LDFLAGS_INTERNAL += -fprofile-generate --coverage
endif

ifneq (,$(findstring verbose,$(D)))
  DEBUG_CFLAGS_INTERNAL += -DDEBUG_VERBOSE_ERROR=1
endif

ifneq (,$(findstring trace,$(D)))
  DEBUG_CFLAGS_INTERNAL += -DDEBUG_TRACE_ON_ERROR=1
endif

ifneq (,$(findstring abort,$(D)))
  DEBUG_CFLAGS_INTERNAL += -DDEBUG_ABORT_ON_ERROR=1
endif

ifneq (,$(findstring all,$(D)))
  DEBUG_CFLAGS_INTERNAL += -DDEBUG_VERBOSE_ERROR=1
  DEBUG_CFLAGS_INTERNAL += -DDEBUG_TRACE_ON_ERROR=1
  DEBUG_CFLAGS_INTERNAL += -DDEBUG_ABORT_ON_ERROR=1
endif

ifneq (,$(findstring asan,$(D)))
  DEBUG_CFLAGS_INTERNAL += -fsanitize=address
  DEBUG_LDFLAGS_INTERNAL += -fsanitize=address -lasan
endif

ifneq (,$(findstring tsan,$(D)))
  DEBUG_CFLAGS_INTERNAL += -fsanitize=thread -fPIC
  DEBUG_LDFLAGS_INTERNAL += -fsanitize=thread -ltsan -pie
endif

ifneq (,$(findstring ubsan,$(D)))
  DEBUG_CFLAGS_INTERNAL += -fsanitize=undefined
  DEBUG_LDFLAGS_INTERNAL += -fsanitize=undefined -lubsan
endif

ifneq (,$(findstring bcheck,$(D)))
  DEBUG_CFLAGS_INTERNAL += -DDEBUG_BUILD_CHECKS
endif

MAKEOPTS = --no-print-directory Q=$(Q)

# built-in sources into "busybox", all files that contain the main function and
# are not compiled standalone
progs_box_main = btrfs.o mkfs/main.o image/main.o convert/main.o \
		 tune/main.o btrfs-find-root.o

progs_box_all_objects = $(mkfs_objects) $(image_objects) $(convert_objects) $(tune_objects)
progs_box_all_static_objects = $(static_mkfs_objects) $(static_image_objects) \
			       $(static_convert_objects) $(static_tune_objects)

progs_box_objects = $(filter-out %/main.o, $(progs_box_all_objects)) \
		    $(patsubst %.o, %.box.o, $(progs_box_main))
progs_box_static_objects = $(filter-out %/main.static.o, $(progs_box_all_static_objects)) \
		    $(patsubst %.o, %.box.static.o, $(progs_box_main))

# Programs to install.
progs_install = btrfs mkfs.btrfs btrfs-map-logical btrfs-image \
		btrfs-find-root btrfstune btrfs-select-super

# Programs to build.
progs_build = $(progs_install) btrfsck btrfs-corrupt-block

# All programs. Use := instead of = so that this is expanded before we reassign
# progs_build below.
progs := $(progs_build) btrfs-convert btrfs-fragments btrfs-sb-mod

ifneq ($(DISABLE_BTRFSCONVERT),1)
progs_install += btrfs-convert
endif

# Static programs to build. Use := instead of = because `make static` should
# still build everything even if --disable-programs was passed to ./configure.
progs_static := $(foreach p,$(progs_build),$(p).static)

ifneq ($(BUILD_PROGRAMS),1)
progs_install =
progs_build =
endif

# external libs required by various binaries; for btrfs-foo,
# specify btrfs_foo_libs = <list of libs>; see $($(subst...)) rules below
btrfs_convert_cflags = -DBTRFSCONVERT_EXT2=$(BTRFSCONVERT_EXT2)
btrfs_convert_cflags += -DBTRFSCONVERT_REISERFS=$(BTRFSCONVERT_REISERFS)
btrfs_fragments_libs = -lgd -lpng -ljpeg -lfreetype
cmds_restore_cflags = -DCOMPRESSION_LZO=$(COMPRESSION_LZO) -DCOMPRESSION_ZSTD=$(COMPRESSION_ZSTD)

ifeq ($(CRYPTOPROVIDER_BUILTIN),1)
CRYPTO_OBJECTS = crypto/sha224-256.o crypto/blake2b-ref.o crypto/blake2b-sse2.o \
		 crypto/blake2b-sse41.o crypto/blake2b-avx2.o crypto/sha256-x86.o
CRYPTO_CFLAGS = -DCRYPTOPROVIDER_BUILTIN=1
endif

ifeq ($(TARGET_CPU),x86_64)
# FIXME: linkage is broken on musl for some reason
ifeq ($(HAVE_GLIBC),1)
CRYPTO_OBJECTS += crypto/crc32c-pcl-intel-asm_64.o
ASFLAGS += -fPIC
endif
endif

CHECKER_FLAGS += $(btrfs_convert_cflags)

# collect values of the variables above
standalone_deps = $(foreach dep,$(patsubst %,%_objects,$(subst -,_,$(filter btrfs-%, $(progs)))),$($(dep)))

SUBDIRS =
BUILDDIRS = $(patsubst %,build-%,$(SUBDIRS))
INSTALLDIRS = $(patsubst %,install-%,$(SUBDIRS))
CLEANDIRS = $(patsubst %,clean-%,$(SUBDIRS))

ifneq ($(DISABLE_DOCUMENTATION),1)
BUILDDIRS += build-Documentation
INSTALLDIRS += install-Documentation
endif

.PHONY: $(SUBDIRS)
.PHONY: $(BUILDDIRS)
.PHONY: $(INSTALLDIRS)
.PHONY: $(TESTDIRS)
.PHONY: $(CLEANDIRS)
.PHONY: all install clean
.PHONY: FORCE

# Create all the static targets
static_objects = $(patsubst %.o, %.static.o, $(objects))
static_cmds_objects = $(patsubst %.o, %.static.o, $(cmds_objects))
static_libbtrfs_objects = $(patsubst %.o, %.static.o, $(shared_objects))
static_libbtrfsutil_objects = $(patsubst %.o, %.static.o, $(libbtrfsutil_objects))
static_convert_objects = $(patsubst %.o, %.static.o, $(convert_objects))
static_mkfs_objects = $(patsubst %.o, %.static.o, $(mkfs_objects))
static_image_objects = $(patsubst %.o, %.static.o, $(image_objects))
static_tune_objects = $(patsubst %.o, %.static.o, $(tune_objects))

libs_shared = libbtrfs.so.0.1 libbtrfsutil.so.$(libbtrfsutil_version)
lib_links = libbtrfs.so.0 libbtrfs.so libbtrfsutil.so.$(libbtrfsutil_major) libbtrfsutil.so
libs_build =
ifeq ($(BUILD_SHARED_LIBRARIES),1)
libs_build += $(libs_shared) $(lib_links)
endif
ifeq ($(BUILD_STATIC_LIBRARIES),1)
libs_build += libbtrfs.a libbtrfsutil.a
endif

# make C=1 to enable sparse
ifdef C
	# We're trying to use sparse against glibc headers which go wild
	# trying to use internal compiler macros to test features.  We
	# copy gcc's and give them to sparse.  But not __SIZE_TYPE__
	# 'cause sparse defines that one.
	#
	dummy := $(shell $(CC) -dM -E -x c - < /dev/null | \
			grep -v __SIZE_TYPE__ > $(check_defs))
	check = $(CHECKER)
	check_echo = echo
else
	check = true
	check_echo = true
endif

# Insert .deps/ to the output path
%.o.d: %.c
	$(Q)mkdir -p $(dir $@).deps/
	$(Q)$(CC) -MM -MG -MF $(dir $@).deps/$(notdir $@) \
		-MT $($(dir $@).deps/$(notdir $@):.o.d=.o) \
		-MT $($(dir $@).deps/$(notdir $@):.o.d=.static.o) \
		-MT $(dir $@).deps/$(notdir $@) $(CFLAGS) $<

.S.o:
	@echo "    [AS]     $@"
	$(Q)$(CC) $(CFLAGS) $(ASFLAGS) -c $< -o $@

%.static.o: %.S
	@echo "    [AS]     $@"
	$(Q)$(CC) $(CFLAGS) $(ASFLAGS) -c $< -o $@
#
# Pick from per-file variables, btrfs_*_cflags
#
.c.o:
	@$(check_echo) "    [SP]     $<"
	$(Q)$(check) $(CFLAGS) $(CHECKER_FLAGS) $<
	@echo "    [CC]     $@"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@ $($(subst /,_,$(subst -,_,$(@:%.o=%)-cflags))) \
		$($(subst -,_,btrfs-$(@:%/$(notdir $@)=%)-cflags))

%.static.o: %.c
	@echo "    [CC]     $@"
	$(Q)$(CC) $(STATIC_CFLAGS) -c $< -o $@ $($(subst /,_,$(subst -,_,$(@:%.static.o=%)-cflags))) \
		$($(subst -,_,btrfs-$(@:%/$(notdir $@)=%)-cflags))

%.box.o: %.c
	@echo "    [CC]     $@"
	$(Q)$(CC) -DENABLE_BOX=1 $(CFLAGS) $(btrfs_convert_cflags) -c $< -o $@

%.box.static.o: %.c
	@echo "    [CC]     $@"
	$(Q)$(CC) -DENABLE_BOX=1 $(STATIC_CFLAGS) $(btrfs_convert_cflags) -c $< -o $@

all: $(progs_build) $(libs_build) $(BUILDDIRS)
ifeq ($(PYTHON_BINDINGS),1)
all: libbtrfsutil_python
endif
$(SUBDIRS): $(BUILDDIRS)
$(BUILDDIRS):
	@echo "Making all in $(patsubst build-%,%,$@)"
	$(Q)$(MAKE) $(MAKEOPTS) -C $(patsubst build-%,%,$@)

test-convert: btrfs btrfs-convert
	@echo "    [TEST]   convert-tests.sh"
	$(Q)bash tests/convert-tests.sh

test-check: test-fsck
test-check-lowmem: test-fsck
test-fsck: btrfs btrfs-image btrfs-corrupt-block mkfs.btrfs btrfstune
ifneq ($(MAKECMDGOALS),test-check-lowmem)
	@echo "    [TEST]   fsck-tests.sh"
	$(Q)bash tests/fsck-tests.sh
else
	@echo "    [TEST]   fsck-tests.sh (mode=lowmem)"
	$(Q)TEST_ENABLE_OVERRIDE=true TEST_ARGS_CHECK=--mode=lowmem bash tests/fsck-tests.sh
endif

test-misc: btrfs btrfs-image btrfs-corrupt-block mkfs.btrfs btrfstune fssum fsstress \
		btrfs-find-root btrfs-select-super btrfs-convert
	@echo "    [TEST]   misc-tests.sh"
	$(Q)bash tests/misc-tests.sh

test-mkfs: btrfs mkfs.btrfs
	@echo "    [TEST]   mkfs-tests.sh"
	$(Q)bash tests/mkfs-tests.sh

test-fuzz: btrfs btrfs-image
	@echo "    [TEST]   fuzz-tests.sh"
	$(Q)bash tests/fuzz-tests.sh

test-cli: btrfs mkfs.btrfs
	@echo "    [TEST]   cli-tests.sh"
	$(Q)bash tests/cli-tests.sh

test-clean:
	@echo "Cleaning tests"
	$(Q)bash tests/clean-tests.sh

test-inst: all
	@tmpdest=`mktemp --tmpdir -d btrfs-progs-inst.XXXXXX` && \
		echo "Test installation to $$tmpdest" && \
		$(MAKE) $(MAKEOPTS) DESTDIR=$$tmpdest install && \
		$(RM) -rf -- $$tmpdest

test-json: json-formatter-test
	@echo "    [TEST]   json formatting"
	@echo | jq
	@{								\
		max=`./json-formatter-test`;				\
		for testno in `seq 1 $$max`; do				\
			echo "    [TEST/json]  $$testno";		\
			./json-formatter-test $$testno | jq >/dev/null; \
		done							\
	}

test-string-table: string-table-test
	@echo "    [TEST]   string-table formatting"
	@{								\
		max=`./string-table-test`;				\
		for testno in `seq 1 $$max`; do				\
			echo "    [TEST/s-t]  $$testno";		\
			./string-table-test $$testno >/dev/null;	\
		done							\
	}

test: test-check test-check-lowmem test-mkfs test-misc test-cli test-convert test-fuzz

testsuite: btrfs-corrupt-block btrfs-find-root btrfs-select-super fssum fsstress
	@echo "Export tests as a package"
	$(Q)cd tests && ./export-testsuite.sh

ifeq ($(PYTHON_BINDINGS),1)
test-libbtrfsutil: libbtrfsutil_python mkfs.btrfs
	$(Q)cd libbtrfsutil/python; \
		LD_LIBRARY_PATH=../.. $(PYTHON) -m unittest discover -v tests

.PHONY: test-libbtrfsutil

test: test-libbtrfsutil
endif

#
# NOTE: For static compiles, you need to have all the required libs
# 	static equivalent available
#
static: $(progs_static) libbtrfs.a libbtrfsutil.a

libbtrfs/version.h: libbtrfs/version.h.in configure.ac
	@echo "    [SH]     $@"
	$(Q)bash ./config.status --silent $@

mktables: kernel-lib/mktables.c
	@echo "    [CC]     $@"
	$(Q)$(CC) $(CFLAGS) $< -o $@

# the target can be regenerated manually using mktables, but a local copy is
# kept so the build process is simpler
kernel-lib/tables.c:
	@echo "    [TABLE]  $@"
	$(Q)./mktables > $@ || ($(RM) -f $@ && exit 1)

libbtrfs.so.0.1: $(libbtrfs_objects) libbtrfs/libbtrfs.sym
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) $(filter %.o,$^) $(LDFLAGS) $(LIBBTRFS_LIBS) \
		-shared -Wl,-soname,libbtrfs.so.0 -Wl,--version-script=libbtrfs/libbtrfs.sym -o $@

libbtrfs.a: $(libbtrfs_objects)
	@echo "    [AR]     $@"
	$(Q)$(AR) cr $@ $^

libbtrfs.so.0 libbtrfs.so: libbtrfs.so.0.1 libbtrfs/libbtrfs.sym
	@echo "    [LN]     $@"
	$(Q)$(LN_S) -f $< $@

libbtrfsutil/%.o: libbtrfsutil/%.c
	@echo "    [CC]     $@"
	$(Q)$(CC) $(LIBBTRFSUTIL_CFLAGS) -o $@ -c $< -o $@

libbtrfsutil.so.$(libbtrfsutil_version): $(libbtrfsutil_objects) libbtrfsutil/libbtrfsutil.sym
	@echo "    [LD]     $@"
	$(Q)$(CC) $(LIBBTRFSUTIL_CFLAGS) $(libbtrfsutil_objects) $(LIBBTRFSUTIL_LDFLAGS) \
		-shared -Wl,-soname,libbtrfsutil.so.$(libbtrfsutil_major) \
		-Wl,--version-script=libbtrfsutil/libbtrfsutil.sym -o $@

libbtrfsutil.a: $(libbtrfsutil_objects)
	@echo "    [AR]     $@"
	$(Q)$(AR) cr $@ $^

libbtrfsutil.so.$(libbtrfsutil_major) libbtrfsutil.so: libbtrfsutil.so.$(libbtrfsutil_version)
	@echo "    [LN]     $@"
	$(Q)$(LN_S) -f $< $@

ifeq ($(PYTHON_BINDINGS),1)
libbtrfsutil_python: libbtrfsutil.so.$(libbtrfsutil_major) libbtrfsutil.so libbtrfsutil/btrfsutil.h
	@echo "    [PY]     libbtrfsutil"
	$(Q)cd libbtrfsutil/python; \
		CFLAGS="$(EXTRA_PYTHON_CFLAGS)" LDFLAGS="$(EXTRA_PYTHON_LDFLAGS)" $(PYTHON) setup.py $(SETUP_PY_Q) build_ext -i build

.PHONY: libbtrfsutil_python
endif

# keep intermediate files from the below implicit rules around
.PRECIOUS: $(addsuffix .o,$(progs))

# Make any btrfs-foo out of btrfs-foo.o, with appropriate libs.
# The $($(subst...)) bits below takes the btrfs_*_libs definitions above and
# turns them into a list of libraries to link against if they exist
#
# For static variants, use an extra $(subst) to get rid of the ".static"
# from the target name before translating to list of libs

btrfs-%.static: btrfs-%.static.o $(static_objects) $(patsubst %.o,%.static.o,$(standalone_deps)) $(static_libbtrfs_objects)
	@echo "    [LD]     $@"
	$(Q)$(CC) -o $@ $@.o $(static_objects) \
		$(patsubst %.o, %.static.o, $($(subst -,_,$(subst .static,,$@)-objects))) \
		$(static_libbtrfs_objects) $(STATIC_LDFLAGS) \
		$($(subst -,_,$(subst .static,,$@)-libs)) $(STATIC_LIBS)

btrfs-%: btrfs-%.o $(objects) $(standalone_deps) libbtrfsutil.a
	@echo "    [LD]     $@"
	$(Q)$(CC) -o $@ $(objects) $@.o \
		$($(subst -,_,$@-objects)) \
		libbtrfsutil.a \
		$(LDFLAGS) $(LIBS) $($(subst -,_,$@-libs))

btrfs: btrfs.o $(objects) $(cmds_objects) libbtrfsutil.a
	@echo "    [LD]     $@"
	$(Q)$(CC) -o $@ $^ $(LDFLAGS) $(LIBS) $(LIBS_COMP)

btrfs.static: btrfs.static.o $(static_objects) $(static_cmds_objects) $(static_libbtrfs_objects) $(static_libbtrfsutil_objects)
	@echo "    [LD]     $@"
	$(Q)$(CC) -o $@ $^ $(STATIC_LDFLAGS) $(STATIC_LIBS) $(STATIC_LIBS_COMP)

btrfs.box: btrfs.box.o $(objects) $(cmds_objects) $(progs_box_objects) libbtrfsutil.a
	@echo "    [LD]     $@"
	$(Q)$(CC) -o $@ $^ $(btrfs_convert_libs) $(LDFLAGS) $(LIBS) $(LIBS_COMP)

btrfs.box.static: btrfs.box.static.o $(static_objects) $(static_cmds_objects) $(progs_box_static_objects) $(static_libbtrfs_objects) $(static_libbtrfsutil_objects)
	@echo "    [LD]     $@"
	$(Q)$(CC) $(STATIC_CFLAGS) -o $@ $^ $(btrfs_convert_libs) \
		$(STATIC_LDFLAGS) $(STATIC_LIBS) $(STATIC_LIBS_COMP)

box-links: btrfs.box
	@echo "    [LN]     mkfs.btrfs"
	$(Q)$(LN_S) -sf btrfs.box mkfs.btrfs
	@echo "    [LN]     btrfs-image"
	$(Q)$(LN_S) -sf btrfs.box btrfs-image
	@echo "    [LN]     btrfs-convert"
	$(Q)$(LN_S) -sf btrfs.box btrfs-convert
	@echo "    [LN]     btrfstune"
	$(Q)$(LN_S) -sf btrfs.box btrfstune

# For backward compatibility, 'btrfs' changes behaviour to fsck if it's named 'btrfsck'
btrfsck: btrfs
	@echo "    [LN]     $@"
	$(Q)$(LN_S) -f btrfs btrfsck

btrfsck.static: btrfs.static
	@echo "    [LN]     $@"
	$(Q)$(LN_S) -f $^ $@

mkfs.btrfs: $(mkfs_objects) $(objects) libbtrfsutil.a
	@echo "    [LD]     $@"
	$(Q)$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)

mkfs.btrfs.static: $(static_mkfs_objects) $(static_objects) $(static_libbtrfs_objects)
	@echo "    [LD]     $@"
	$(Q)$(CC) -o $@ $^ $(STATIC_LDFLAGS) $(STATIC_LIBS)

btrfstune: $(tune_objects) $(objects) libbtrfsutil.a
	@echo "    [LD]     $@"
	$(Q)$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)

btrfstune.static: $(static_tune_objects) $(static_objects) $(static_libbtrfs_objects)
	@echo "    [LD]     $@"
	$(Q)$(CC) -o $@ $^ $(STATIC_LDFLAGS) $(STATIC_LIBS)

btrfs-image: $(image_objects) $(objects) libbtrfsutil.a
	@echo "    [LD]     $@"
	$(Q)$(CC) -o $@ $^ $(LDFLAGS) $(LIBS) $(LIBS_COMP)

btrfs-image.static: $(static_image_objects) $(static_objects) $(static_libbtrfs_objects)
	@echo "    [LD]     $@"
	$(Q)$(CC) -o $@ $^ $(STATIC_LDFLAGS) $(STATIC_LIBS) $(STATIC_LIBS_COMP)

btrfs-convert: $(convert_objects) $(objects) libbtrfsutil.a
	@echo "    [LD]     $@"
	$(Q)$(CC) -o $@ $^ $(LDFLAGS) $(btrfs_convert_libs) $(LIBS)

btrfs-convert.static: $(static_convert_objects) $(static_objects) $(static_libbtrfs_objects)
	@echo "    [LD]     $@"
	$(Q)$(CC) -o $@ $^ $(STATIC_LDFLAGS) $(btrfs_convert_libs) $(STATIC_LIBS)

quick-test: quick-test.o $(objects) libbtrfsutil.a $(libs_shared)
	@echo "    [LD]     $@"
	$(Q)$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)

ioctl-test.o: tests/ioctl-test.c kernel-shared/uapi/btrfs.h include/kerncompat.h kernel-shared/ctree.h
	@echo "    [CC]     $@"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

ioctl-test-32.o: tests/ioctl-test.c kernel-shared/uapi/btrfs.h include/kerncompat.h kernel-shared/ctree.h
	@echo "    [CC32]   $@"
	$(Q)$(CC) $(CFLAGS) -m32 -c $< -o $@

ioctl-test-64.o: tests/ioctl-test.c kernel-shared/uapi/btrfs.h include/kerncompat.h kernel-shared/ctree.h
	@echo "    [CC64]   $@"
	$(Q)$(CC) $(CFLAGS) -m64 -c $< -o $@

ioctl-test: ioctl-test.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "   ?[PAHOLE] $@.pahole"
	-$(Q)pahole $@ > $@.pahole

ioctl-test-32: ioctl-test-32.o
	@echo "    [LD32]   $@"
	$(Q)$(CC) -m32 -o $@ $< $(LDFLAGS)
	@echo "   ?[PAHOLE] $@.pahole"
	-$(Q)pahole $@ > $@.pahole

ioctl-test-64: ioctl-test-64.o
	@echo "    [LD64]   $@"
	$(Q)$(CC) -m64 -o $@ $< $(LDFLAGS)
	@echo "   ?[PAHOLE] $@.pahole"
	-$(Q)pahole $@ > $@.pahole

test-ioctl: ioctl-test ioctl-test-32 ioctl-test-64
	@echo "    [TEST/ioctl]"
	$(Q)./ioctl-test > ioctl-test.log
	$(Q)./ioctl-test-32 > ioctl-test-32.log
	$(Q)./ioctl-test-64 > ioctl-test-64.log

library-test: tests/library-test.c libbtrfs.so
	@echo "    [TEST PREP]  $@"$(eval TMPD=$(shell mktemp -d))
	$(Q)mkdir -p $(TMPD)/include/btrfs && \
	cp $(libbtrfs_headers) $(TMPD)/include/btrfs && \
	cp libbtrfs.so.0.1 $(TMPD) && \
	cd $(TMPD) && $(CC) -I$(TMPD)/include -o $@ $(addprefix $(ABSTOPDIR)/,$^) -Wl,-rpath=$(ABSTOPDIR)
	@echo "    [TEST RUN]   $@"
	$(Q)cd $(TMPD) && LD_PRELOAD=libbtrfs.so.0.1 ./$@
	@echo "    [TEST CLEAN] $@"
	$(Q)$(RM) -rf -- $(TMPD)

library-test.static: tests/library-test.c libbtrfs.a libbtrfsutil.a
	@echo "    [TEST PREP]  $@"$(eval TMPD=$(shell mktemp -d))
	$(Q)mkdir -p $(TMPD)/include/btrfs && \
	cp $(libbtrfs_headers) $(TMPD)/include/btrfs && \
	cd $(TMPD) && $(CC) -I$(TMPD)/include -o $@ $(addprefix $(ABSTOPDIR)/,$^) $(STATIC_LDFLAGS) $(STATIC_LIBS)
	@echo "    [TEST RUN]   $@"
	$(Q)cd $(TMPD) && ./$@
	@echo "    [TEST CLEAN] $@"
	$(Q)$(RM) -rf -- $(TMPD)

fssum: tests/fssum.c crypto/sha224-256.c crypto/sha256-x86.o common/cpu-utils.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

fsstress: tests/fsstress.c
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -luring -laio

hash-speedtest: crypto/hash-speedtest.c $(objects) libbtrfsutil.a
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

hash-vectest: crypto/hash-vectest.c $(objects) libbtrfsutil.a
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

json-formatter-test: tests/json-formatter-test.c $(objects) libbtrfsutil.a
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

string-table-test: tests/string-table-test.c $(objects) libbtrfsutil.a
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

test-build: test-build-pre test-build-real

test-build-pre:
	$(MAKE) $(MAKEOPTS) clean-all
	./autogen.sh
	./configure

test-build-real:
	$(MAKE) $(MAKEOPTS) library-test
	-$(MAKE) $(MAKEOPTS) library-test.static
	$(MAKE) $(MAKEOPTS) -j 8 $(progs) libbtrfs.a libbtrfsutil.a $(libs_shared) $(lib_links) $(BUILDDIRS)
	-$(MAKE) $(MAKEOPTS) -j 8 static

manpages:
	$(Q)$(MAKE) $(MAKEOPTS) -C Documentation

tags: FORCE
	@echo "    [TAGS]   $(TAGS_CMD)"
	$(Q)$(TAGS_CMD) *.[ch] image/*.[ch] convert/*.[ch] mkfs/*.[ch] \
		check/*.[ch] kernel-lib/*.[ch] kernel-shared/*.[ch] \
		cmds/*.[ch] common/*.[ch] tune/*.[ch] \
		libbtrfsutil/*.[ch]

etags: FORCE
	@echo "    [ETAGS]   $(ETAGS_CMD)"
	$(Q)$(ETAGS_CMD) *.[ch] image/*.[ch] convert/*.[ch] mkfs/*.[ch] \
		check/*.[ch] kernel-lib/*.[ch] kernel-shared/*.[ch] \
		cmds/*.[ch] common/*.[ch] tune/*.[ch] \
		libbtrfsutil/*.[ch]

cscope: FORCE
	@echo "    [CSCOPE] $(CSCOPE_CMD)"
	$(Q)ls -1 *.[ch] image/*.[ch] convert/*.[ch] mkfs/*.[ch] check/*.[ch] \
		kernel-lib/*.[ch] kernel-shared/*.[ch] libbtrfsutil/*.[ch] \
		cmds/*.[ch] common/*.[ch] tune/*.[ch] \
		> cscope.files
	$(Q)$(CSCOPE_CMD)

clean-all: clean clean-doc clean-gen

clean: $(CLEANDIRS)
	@echo "Cleaning"
	$(Q)$(RM) -f -- $(progs) *.o .deps/*.o.d \
		kernel-lib/*.o kernel-lib/.deps/*.o.d \
		kernel-shared/*.o kernel-shared/.deps/*.o.d \
		image/*.o image/.deps/*.o.d \
		convert/*.o convert/.deps/*.o.d \
		mkfs/*.o mkfs/.deps/*.o.d check/*.o check/.deps/*.o.d \
		cmds/*.o cmds/.deps/*.o.d common/*.o common/.deps/*.o.d \
		crypto/*.o crypto/.deps/*.o.d \
		tune/*.o tune/.deps/*.o.d \
		libbtrfs/*.o libbtrfs/.deps/*.o.d \
		*.gcno *.gcda *.gcov */*.gcno */*.gcda */*/.gcov \
	      ioctl-test quick-test library-test library-test-static \
              mktables btrfs.static mkfs.btrfs.static fssum \
	      btrfs.box btrfs.box.static json-formatter-test \
	      hash-speedtest \
	      $(check_defs) \
	      libbtrfs.a libbtrfsutil.a $(libs_shared) $(lib_links) \
	      $(progs_static) \
	      libbtrfsutil/*.o libbtrfsutil/.deps/*.o.d
	$(Q)$(RM) -fd -- .deps */.deps */*/.deps
ifeq ($(PYTHON_BINDINGS),1)
	$(Q)cd libbtrfsutil/python; \
		$(PYTHON) setup.py $(SETUP_PY_Q) clean -a
endif

clean-doc:
	@echo "Cleaning Documentation"
	$(Q)$(MAKE) $(MAKEOPTS) -C Documentation clean

clean-gen:
	@echo "Cleaning Generated Files"
	$(Q)$(RM) -rf -- libbtrfs/version.h config.status config.cache config.log \
		configure.lineno config.status.lineno Makefile.inc \
		Documentation/Makefile tags TAGS \
		cscope.files cscope.out cscope.in.out cscope.po.out \
		config.log include/config.h include/config.h.in~ aclocal.m4 \
		configure autom4te.cache/

clean-dep:
	@echo "Cleaning dependency files"
	$(Q)$(RM) -f -- *.o.d */*.o.d */*/*.o.d \
		.deps/*.o.d */.deps/*.o.d */*/.deps/*.o.d
	$(Q)$(RM) -fd -- .deps */.deps */*/.deps

$(CLEANDIRS):
	@echo "Cleaning $(patsubst clean-%,%,$@)"
	$(Q)$(MAKE) $(MAKEOPTS) -C $(patsubst clean-%,%,$@) clean

install: $(libs_build) $(progs_install) $(INSTALLDIRS)
ifeq ($(BUILD_PROGRAMS),1)
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(progs_install) $(DESTDIR)$(bindir)
	$(INSTALL) fsck.btrfs $(DESTDIR)$(bindir)
	# btrfsck is a link to btrfs in the src tree, make it so for installed file as well
	$(LN_S) -f btrfs $(DESTDIR)$(bindir)/btrfsck
ifneq ($(udevdir),)
	$(INSTALL) -m755 -d $(DESTDIR)$(udevruledir)
	$(INSTALL) -m644 $(udev_rules) $(DESTDIR)$(udevruledir)
endif
endif
ifneq ($(libs_build),)
	$(INSTALL) -m755 -d $(DESTDIR)$(libdir)
	$(INSTALL) $(libs_build) $(DESTDIR)$(libdir)
ifeq ($(BUILD_SHARED_LIBRARIES),1)
	cp -d $(lib_links) $(DESTDIR)$(libdir)
endif
	$(INSTALL) -m755 -d $(DESTDIR)$(incdir)/btrfs
	$(INSTALL) -m644 $(libbtrfs_headers) $(DESTDIR)$(incdir)/btrfs
	$(INSTALL) -m644 libbtrfsutil/btrfsutil.h $(DESTDIR)$(incdir)
	$(INSTALL) -m755 -d $(DESTDIR)$(pkgconfigdir)
	$(INSTALL) -m644 libbtrfsutil/libbtrfsutil.pc $(DESTDIR)$(pkgconfigdir)
endif

ifeq ($(PYTHON_BINDINGS),1)
install_python: libbtrfsutil_python
	$(Q)cd libbtrfsutil/python; \
		$(PYTHON) setup.py install --skip-build $(if $(DESTDIR),--root $(DESTDIR)) --prefix $(prefix)

.PHONY: install_python
endif

install-static: $(progs_static) $(INSTALLDIRS)
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(progs_static) $(DESTDIR)$(bindir)
	# btrfsck is a link to btrfs in the src tree, make it so for installed file as well
	$(LN_S) -f btrfs.static $(DESTDIR)$(bindir)/btrfsck.static
	$(INSTALL) -m755 -d $(DESTDIR)$(libdir)
	$(INSTALL) libbtrfs.a libbtrfsutil.a $(DESTDIR)$(libdir)
	$(INSTALL) -m755 -d $(DESTDIR)$(incdir)/btrfs
	$(INSTALL) -m644 $(libbtrfs_headers) $(DESTDIR)$(incdir)/btrfs

$(INSTALLDIRS):
	@echo "Making install in $(patsubst install-%,%,$@)"
	$(Q)$(MAKE) $(MAKEOPTS) -C $(patsubst install-%,%,$@) install

uninstall:
	$(Q)$(MAKE) $(MAKEOPTS) -C Documentation uninstall
	cd $(DESTDIR)$(incdir)/btrfs; $(RM) -f -- $(libbtrfs_headers)
	$(RMDIR) -p --ignore-fail-on-non-empty -- $(DESTDIR)$(incdir)/btrfs
	cd $(DESTDIR)$(incdir); $(RM) -f -- btrfsutil.h
	cd $(DESTDIR)$(libdir); $(RM) -f -- $(lib_links) libbtrfs.a libbtrfsutil.a $(libs_shared)
	cd $(DESTDIR)$(bindir); $(RM) -f -- btrfsck fsck.btrfs $(progs_install)

ifneq ($(MAKECMDGOALS),clean)
-include $(all_objects:.o=.o.d) $(subst .btrfs,, $(filter-out btrfsck.o.d, $(progs:=.o.d)))
endif
