# Export all variables to sub-makes by default
export

CC = gcc
LN = ln
AR = ar
AM_CFLAGS = -Wall -D_FILE_OFFSET_BITS=64 -DBTRFS_FLAT_INCLUDES -fPIC
CFLAGS = -g -O1
objects = ctree.o disk-io.o radix-tree.o extent-tree.o print-tree.o \
	  root-tree.o dir-item.o file-item.o inode-item.o inode-map.o \
	  extent-cache.o extent_io.o volumes.o utils.o repair.o \
	  qgroup.o raid6.o free-space-cache.o list_sort.o
cmds_objects = cmds-subvolume.o cmds-filesystem.o cmds-device.o cmds-scrub.o \
	       cmds-inspect.o cmds-balance.o cmds-send.o cmds-receive.o \
	       cmds-quota.o cmds-qgroup.o cmds-replace.o cmds-check.o \
	       cmds-restore.o cmds-rescue.o chunk-recover.o super-recover.o
libbtrfs_objects = send-stream.o send-utils.o rbtree.o btrfs-list.o crc32c.o \
		   uuid-tree.o
libbtrfs_headers = send-stream.h send-utils.h send.h rbtree.h btrfs-list.h \
	       crc32c.h list.h kerncompat.h radix-tree.h extent-cache.h \
	       extent_io.h ioctl.h ctree.h btrfsck.h
TESTS = fsck-tests.sh

INSTALL = install
prefix ?= /usr/local
bindir = $(prefix)/bin
lib_LIBS = -luuid -lblkid -lm -lz -llzo2 -L.
libdir ?= $(prefix)/lib
incdir = $(prefix)/include/btrfs
LIBS = $(lib_LIBS) $(libs_static)

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

progs = mkfs.btrfs btrfs-debug-tree btrfsck \
	btrfs btrfs-map-logical btrfs-image btrfs-zero-log btrfs-convert \
	btrfs-find-root btrfstune btrfs-show-super

# external libs required by various binaries; for btrfs-foo,
# specify btrfs_foo_libs = <list of libs>; see $($(subst...)) rules below
btrfs_convert_libs = -lext2fs -lcom_err
btrfs_image_libs = -lpthread
btrfs_fragment_libs = -lgd -lpng -ljpeg -lfreetype

SUBDIRS = man
BUILDDIRS = $(patsubst %,build-%,$(SUBDIRS))
INSTALLDIRS = $(patsubst %,install-%,$(SUBDIRS))
CLEANDIRS = $(patsubst %,clean-%,$(SUBDIRS))

.PHONY: $(SUBDIRS)
.PHONY: $(BUILDDIRS)
.PHONY: $(INSTALLDIRS)
.PHONY: $(TESTDIRS)
.PHONY: $(CLEANDIRS)
.PHONY: all install clean

# Create all the static targets
static_objects = $(patsubst %.o, %.static.o, $(objects))
static_cmds_objects = $(patsubst %.o, %.static.o, $(cmds_objects))
static_libbtrfs_objects = $(patsubst %.o, %.static.o, $(libbtrfs_objects))

# Define static compilation flags
STATIC_CFLAGS = $(CFLAGS) -ffunction-sections -fdata-sections
STATIC_LDFLAGS = -static -Wl,--gc-sections
STATIC_LIBS = $(lib_LIBS) -lpthread

libs_shared = libbtrfs.so.0.1
libs_static = libbtrfs.a
libs = $(libs_shared) $(libs_static)
lib_links = libbtrfs.so.0 libbtrfs.so
headers = $(libbtrfs_headers)

# make C=1 to enable sparse
check_defs := .cc-defines.h 
ifdef C
	#
	# We're trying to use sparse against glibc headers which go wild
	# trying to use internal compiler macros to test features.  We
	# copy gcc's and give them to sparse.  But not __SIZE_TYPE__
	# 'cause sparse defines that one.
	#
	dummy := $(shell $(CC) -dM -E -x c - < /dev/null | \
			grep -v __SIZE_TYPE__ > $(check_defs))
	check = sparse -include $(check_defs) -D__CHECKER__ \
		-D__CHECK_ENDIAN__ -Wbitwise -Wuninitialized -Wshadow -Wundef
	check_echo = echo
	# don't use FORTIFY with sparse because glibc with FORTIFY can
	# generate so many sparse errors that sparse stops parsing,
	# which masks real errors that we want to see.
else
	check = true
	check_echo = true
	AM_CFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
endif

%.o.d: %.c
	$(Q)$(CC) -MM -MG -MF $@ -MT $(@:.o.d=.o) -MT $(@:.o.d=.static.o) -MT $@ $(AM_CFLAGS) $(CFLAGS) $<

.c.o:
	@$(check_echo) "    [SP]     $<"
	$(Q)$(check) $(AM_CFLAGS) $(CFLAGS) $<
	@echo "    [CC]     $@"
	$(Q)$(CC) $(AM_CFLAGS) $(CFLAGS) -c $<

%.static.o: %.c
	@echo "    [CC]     $@"
	$(Q)$(CC) $(AM_CFLAGS) $(STATIC_CFLAGS) -c $< -o $@

all: $(progs) manpages $(BUILDDIRS)
$(SUBDIRS): $(BUILDDIRS)
$(BUILDDIRS):
	@echo "Making all in $(patsubst build-%,%,$@)"
	$(Q)$(MAKE) $(MAKEOPTS) -C $(patsubst build-%,%,$@)

test:
	$(Q)for t in $(TESTS); do \
		echo "     [TEST]    $$t"; \
		bash tests/$$t || exit 1; \
	done

#
# NOTE: For static compiles, you need to have all the required libs
# 	static equivalent available
#
static: btrfs.static mkfs.btrfs.static btrfs-find-root.static

version.h:
	@echo "    [SH]     $@"
	$(Q)bash version.sh

$(libs_shared): $(libbtrfs_objects) $(lib_links) send.h
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) $(libbtrfs_objects) $(LDFLAGS) $(lib_LIBS) \
		-shared -Wl,-soname,libbtrfs.so.0 -o libbtrfs.so.0.1

$(libs_static): $(libbtrfs_objects)
	@echo "    [AR]     $@"
	$(Q)$(AR) cru libbtrfs.a $(libbtrfs_objects)

$(lib_links):
	@echo "    [LN]     $@"
	$(Q)$(LN) -sf libbtrfs.so.0.1 libbtrfs.so.0
	$(Q)$(LN) -sf libbtrfs.so.0.1 libbtrfs.so

# keep intermediate files from the below implicit rules around
.PRECIOUS: $(addsuffix .o,$(progs))

# Make any btrfs-foo out of btrfs-foo.o, with appropriate libs.
# The $($(subst...)) bits below takes the btrfs_*_libs definitions above and
# turns them into a list of libraries to link against if they exist
#
# For static variants, use an extra $(subst) to get rid of the ".static"
# from the target name before translating to list of libs

btrfs-%.static: $(static_objects) btrfs-%.static.o $(static_libbtrfs_objects)
	@echo "    [LD]     $@"
	$(Q)$(CC) $(STATIC_CFLAGS) -o $@ $@.o $(static_objects) \
		$(static_libbtrfs_objects) $(STATIC_LDFLAGS) $(STATIC_LIBS) \
		$($(subst -,_,$(subst .static,,$@)-libs))

btrfs-%: $(objects) $(libs) btrfs-%.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $(objects) $@.o $(LDFLAGS) $(LIBS) $($(subst -,_,$@-libs))

btrfs: $(objects) btrfs.o help.o $(cmds_objects) $(libs)
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfs btrfs.o help.o $(cmds_objects) \
		$(objects) $(LDFLAGS) $(LIBS) -lpthread

btrfs.static: $(static_objects) btrfs.static.o help.static.o $(static_cmds_objects) $(static_libbtrfs_objects)
	@echo "    [LD]     $@"
	$(Q)$(CC) $(STATIC_CFLAGS) -o btrfs.static btrfs.static.o help.static.o $(static_cmds_objects) \
		$(static_objects) $(static_libbtrfs_objects) $(STATIC_LDFLAGS) $(STATIC_LIBS)

# For backward compatibility, 'btrfs' changes behaviour to fsck if it's named 'btrfsck'
btrfsck: btrfs
	@echo "    [LN]     $@"
	$(Q)$(LN) -f btrfs btrfsck

mkfs.btrfs: $(objects) $(libs) mkfs.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o mkfs.btrfs $(objects) mkfs.o $(LDFLAGS) $(LIBS)

mkfs.btrfs.static: $(static_objects) mkfs.static.o $(static_libbtrfs_objects)
	@echo "    [LD]     $@"
	$(Q)$(CC) $(STATIC_CFLAGS) -o mkfs.btrfs.static mkfs.static.o $(static_objects) \
		$(static_libbtrfs_objects) $(STATIC_LDFLAGS) $(STATIC_LIBS)

btrfstune: $(objects) $(libs) btrfstune.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfstune $(objects) btrfstune.o $(LDFLAGS) $(LIBS)

dir-test: $(objects) $(libs) dir-test.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o dir-test $(objects) dir-test.o $(LDFLAGS) $(LIBS)

quick-test: $(objects) $(libs) quick-test.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o quick-test $(objects) quick-test.o $(LDFLAGS) $(LIBS)

ioctl-test: $(objects) $(libs) ioctl-test.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o ioctl-test $(objects) ioctl-test.o $(LDFLAGS) $(LIBS)

send-test: $(objects) $(libs) send-test.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o send-test $(objects) send-test.o $(LDFLAGS) $(LIBS) -lpthread

manpages:
	$(Q)$(MAKE) $(MAKEOPTS) -C man

clean: $(CLEANDIRS)
	@echo "Cleaning"
	$(Q)rm -f $(progs) cscope.out *.o *.o.d btrfs-convert btrfs-image btrfs-select-super \
	      btrfs-zero-log btrfstune dir-test ioctl-test quick-test send-test btrfsck \
	      btrfs.static mkfs.btrfs.static btrfs-calc-size \
	      version.h $(check_defs) \
	      $(libs) $(lib_links)

$(CLEANDIRS):
	@echo "Cleaning $(patsubst clean-%,%,$@)"
	$(Q)$(MAKE) $(MAKEOPTS) -C $(patsubst clean-%,%,$@) clean

install: $(libs) $(progs) $(INSTALLDIRS)
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(progs) $(DESTDIR)$(bindir)
	# btrfsck is a link to btrfs in the src tree, make it so for installed file as well
	$(LN) -f $(DESTDIR)$(bindir)/btrfs $(DESTDIR)$(bindir)/btrfsck
	$(INSTALL) -m755 -d $(DESTDIR)$(libdir)
	$(INSTALL) $(libs) $(DESTDIR)$(libdir)
	cp -a $(lib_links) $(DESTDIR)$(libdir)
	$(INSTALL) -m755 -d $(DESTDIR)$(incdir)
	$(INSTALL) -m644 $(headers) $(DESTDIR)$(incdir)

$(INSTALLDIRS):
	@echo "Making install in $(patsubst install-%,%,$@)"
	$(Q)$(MAKE) $(MAKEOPTS) -C $(patsubst install-%,%,$@) install

ifneq ($(MAKECMDGOALS),clean)
-include $(objects:.o=.o.d) $(cmd-objects:.o=.o.d) $(subst .btrfs,, $(filter-out btrfsck.o.d, $(progs:=.o.d)))
endif
