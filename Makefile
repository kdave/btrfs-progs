CC = gcc
LN = ln
AR = ar
AM_CFLAGS = -Wall -D_FILE_OFFSET_BITS=64 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -DBTRFS_FLAT_INCLUDES -fPIC
CFLAGS = -g -O1
objects = ctree.o disk-io.o radix-tree.o extent-tree.o print-tree.o \
	  root-tree.o dir-item.o file-item.o inode-item.o \
	  inode-map.o extent-cache.o extent_io.o \
	  volumes.o utils.o btrfs-list.o repair.o \
	  send-stream.o send-utils.o qgroup.o raid6.o
cmds_objects = cmds-subvolume.o cmds-filesystem.o cmds-device.o cmds-scrub.o \
	       cmds-inspect.o cmds-balance.o cmds-send.o cmds-receive.o \
	       cmds-quota.o cmds-qgroup.o cmds-replace.o cmds-check.o \
	       cmds-restore.o
libbtrfs_objects = send-stream.o send-utils.o rbtree.o btrfs-list.o crc32c.o
libbtrfs_headers = send-stream.h send-utils.h send.h rbtree.h btrfs-list.h \
	       crc32c.h list.h kerncompat.h radix-tree.h extent-cache.h \
	       extent_io.h ioctl.h ctree.h

CHECKFLAGS= -D__linux__ -Dlinux -D__STDC__ -Dunix -D__unix__ -Wbitwise \
	    -Wuninitialized -Wshadow -Wundef
DEPFLAGS = -Wp,-MMD,$(@D)/.$(@F).d,-MT,$@

INSTALL = install
prefix ?= /usr/local
bindir = $(prefix)/bin
lib_LIBS = -luuid -lblkid -lm -lz -L.
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

progs = btrfsctl mkfs.btrfs btrfs-debug-tree btrfs-show btrfs-vol btrfsck \
	btrfs btrfs-map-logical btrfs-image btrfs-zero-log btrfs-convert \
	btrfs-find-root btrfstune btrfs-show-super

# Create all the static targets
static_objects = $(patsubst %.o, %.static.o, $(objects))
static_cmds_objects = $(patsubst %.o, %.static.o, $(cmds_objects))
static_progs = $(patsubst %.o, %.static.o, $(progs))

# Define static compilation flags
STATIC_CFLAGS = $(CFLAGS) -ffunction-sections -fdata-sections
STATIC_LDFLAGS = -static -Wl,--gc-sections
STATIC_LIBS = $(LIBS) -lpthread

libs_shared = libbtrfs.so.0.1
libs_static = libbtrfs.a
libs = $(libs_shared) $(libs_static)
lib_links = libbtrfs.so.0 libbtrfs.so
headers = $(libbtrfs_headers)

# make C=1 to enable sparse
ifdef C
	check = sparse $(CHECKFLAGS)
else
	check = true
endif

.c.o:
	$(Q)$(check) $<
	@echo "    [CC]     $@"
	$(Q)$(CC) $(DEPFLAGS) $(AM_CFLAGS) $(CFLAGS) -c $<

%.static.o: %.c
	@echo "    [CC]     $@"
	$(Q)$(CC) $(DEPFLAGS) $(AM_CFLAGS) $(STATIC_CFLAGS) -c $< -o $@

all: version.h $(progs) manpages

#
# NOTE: For static compiles, you need to have all the required libs
# 	static equivalent available
#
static: version.h $(libs) btrfs.static mkfs.btrfs.static

version.h:
	$(Q)bash version.sh

$(libs_shared): $(libbtrfs_objects) $(lib_links) send.h
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) $(libbtrfs_objects) $(lib_LIBS) -shared -Wl,-soname,libbtrfs.so -o libbtrfs.so.0.1

$(libs_static): $(libbtrfs_objects)
	@echo "    [AR]     $@"
	$(Q)$(AR) cru libbtrfs.a $(libbtrfs_objects)

$(lib_links):
	@echo "    [LN]     $@"
	$(Q)$(LN) -sf libbtrfs.so.0.1 libbtrfs.so.0
	$(Q)$(LN) -sf libbtrfs.so.0.1 libbtrfs.so

btrfs: $(objects) btrfs.o help.o $(cmds_objects) $(libs)
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfs btrfs.o help.o $(cmds_objects) \
		$(objects) $(LDFLAGS) $(LIBS) -lpthread

btrfs.static: $(static_objects) $(libs) btrfs.static.o help.static.o $(static_cmds_objects)
	@echo "    [LD]     $@"
	$(Q)$(CC) $(STATIC_CFLAGS) -o btrfs.static btrfs.static.o help.static.o $(static_cmds_objects) \
		$(static_objects) $(STATIC_LDFLAGS) $(STATIC_LIBS)

calc-size: $(objects) $(libs) calc-size.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o calc-size calc-size.o $(objects) $(LDFLAGS) $(LIBS)

btrfs-find-root: $(objects) $(libs) find-root.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfs-find-root find-root.o $(objects) $(LDFLAGS) $(LIBS)

btrfsctl: $(objects) $(libs) btrfsctl.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfsctl btrfsctl.o $(objects) $(LDFLAGS) $(LIBS)

btrfs-vol: $(objects) $(libs) btrfs-vol.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfs-vol btrfs-vol.o $(objects) $(LDFLAGS) $(LIBS)

btrfs-show: $(objects) $(libs) btrfs-show.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfs-show btrfs-show.o $(objects) $(LDFLAGS) $(LIBS)

# For backward compatibility, 'btrfs' changes behaviour to fsck if it's named 'btrfsck'
btrfsck: btrfs
	@echo "    [LN]     $@"
	$(Q)$(LN) -f btrfs btrfsck

mkfs.btrfs: $(objects) $(libs) mkfs.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o mkfs.btrfs $(objects) mkfs.o $(LDFLAGS) $(LIBS) -lblkid

mkfs.btrfs.static: $(static_objects) mkfs.static.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(STATIC_CFLAGS) -o mkfs.btrfs.static mkfs.static.o \
		$(static_objects) $(STATIC_LDFLAGS) $(STATIC_LIBS)

btrfs-debug-tree: $(objects) $(libs) debug-tree.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfs-debug-tree $(objects) debug-tree.o $(LDFLAGS) $(LIBS)

btrfs-zero-log: $(objects) $(libs) btrfs-zero-log.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfs-zero-log $(objects) btrfs-zero-log.o $(LDFLAGS) $(LIBS)

btrfs-show-super: $(objects) $(libs) btrfs-show-super.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfs-show-super $(objects) btrfs-show-super.o $(LDFLAGS) $(LIBS)

btrfs-select-super: $(objects) $(libs) btrfs-select-super.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfs-select-super $(objects) btrfs-select-super.o $(LDFLAGS) $(LIBS)

btrfstune: $(objects) $(libs) btrfstune.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfstune $(objects) btrfstune.o $(LDFLAGS) $(LIBS)

btrfs-map-logical: $(objects) $(libs) btrfs-map-logical.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfs-map-logical $(objects) btrfs-map-logical.o $(LDFLAGS) $(LIBS)

btrfs-corrupt-block: $(objects) $(libs) btrfs-corrupt-block.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfs-corrupt-block $(objects) btrfs-corrupt-block.o $(LDFLAGS) $(LIBS)

btrfs-image: $(objects) $(libs) btrfs-image.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfs-image $(objects) btrfs-image.o -lpthread -lz $(LDFLAGS) $(LIBS)

dir-test: $(objects) $(libs) dir-test.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o dir-test $(objects) dir-test.o $(LDFLAGS) $(LIBS)

quick-test: $(objects) $(libs) quick-test.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o quick-test $(objects) quick-test.o $(LDFLAGS) $(LIBS)

btrfs-convert: $(objects) $(libs) convert.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfs-convert $(objects) convert.o -lext2fs -lcom_err $(LDFLAGS) $(LIBS)

ioctl-test: $(objects) $(libs) ioctl-test.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o ioctl-test $(objects) ioctl-test.o $(LDFLAGS) $(LIBS)

send-test: $(objects) send-test.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o send-test $(objects) send-test.o $(LDFLAGS) $(LIBS) -lpthread

manpages:
	$(Q)$(MAKE) $(MAKEOPTS) -C man

install-man:
	cd man; $(MAKE) install

clean :
	@echo "Cleaning"
	$(Q)rm -f $(progs) cscope.out *.o .*.d btrfs-convert btrfs-image btrfs-select-super \
	      btrfs-zero-log btrfstune dir-test ioctl-test quick-test send-test btrfsck \
	      btrfs.static mkfs.btrfs.static \
	      version.h \
	      $(libs) $(lib_links)
	$(Q)$(MAKE) $(MAKEOPTS) -C man $@

install: $(libs) $(progs) install-man
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(progs) $(DESTDIR)$(bindir)
	$(INSTALL) -m755 -d $(DESTDIR)$(libdir)
	$(INSTALL) $(libs) $(DESTDIR)$(libdir)
	cp -a $(lib_links) $(DESTDIR)$(libdir)
	$(INSTALL) -m755 -d $(DESTDIR)$(incdir)
	$(INSTALL) -m644 $(headers) $(DESTDIR)$(incdir)

-include .*.d
