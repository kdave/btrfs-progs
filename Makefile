CC = gcc
AM_CFLAGS = -Wall -D_FILE_OFFSET_BITS=64 -D_FORTIFY_SOURCE=2
CFLAGS = -g -O1
objects = ctree.o disk-io.o radix-tree.o extent-tree.o print-tree.o \
	  root-tree.o dir-item.o file-item.o inode-item.o \
	  inode-map.o crc32c.o rbtree.o extent-cache.o extent_io.o \
	  volumes.o utils.o btrfs-list.o btrfslabel.o repair.o \
	  send-stream.o send-utils.o qgroup.o
cmds_objects = cmds-subvolume.o cmds-filesystem.o cmds-device.o cmds-scrub.o \
	       cmds-inspect.o cmds-balance.o cmds-send.o cmds-receive.o \
	       cmds-quota.o cmds-qgroup.o

CHECKFLAGS= -D__linux__ -Dlinux -D__STDC__ -Dunix -D__unix__ -Wbitwise \
	    -Wuninitialized -Wshadow -Wundef
DEPFLAGS = -Wp,-MMD,$(@D)/.$(@F).d,-MT,$@

INSTALL = install
prefix ?= /usr/local
bindir = $(prefix)/bin
LIBS=-luuid -lm
RESTORE_LIBS=-lz

progs = btrfsctl mkfs.btrfs btrfs-debug-tree btrfs-show btrfs-vol btrfsck \
	btrfs btrfs-map-logical btrfs-image btrfs-zero-log btrfs-convert \
	btrfs-find-root btrfs-restore btrfstune

# make C=1 to enable sparse
ifdef C
	check = sparse $(CHECKFLAGS)
else
	check = ls
endif

.c.o:
	$(check) $<
	$(CC) $(DEPFLAGS) $(AM_CFLAGS) $(CFLAGS) -c $<


all: version $(progs) manpages

version:
	bash version.sh

btrfs: $(objects) btrfs.o help.o common.o $(cmds_objects)
	$(CC) $(CFLAGS) -o btrfs btrfs.o help.o common.o $(cmds_objects) \
		$(objects) $(LDFLAGS) $(LIBS) -lpthread

calc-size: $(objects) calc-size.o
	$(CC) $(CFLAGS) -o calc-size calc-size.o $(objects) $(LDFLAGS) $(LIBS)

btrfs-find-root: $(objects) find-root.o
	$(CC) $(CFLAGS) -o btrfs-find-root find-root.o $(objects) $(LDFLAGS) $(LIBS)

btrfs-restore: $(objects) restore.o
	$(CC) $(CFLAGS) -o btrfs-restore restore.o $(objects) $(LDFLAGS) $(LIBS) $(RESTORE_LIBS)

btrfsctl: $(objects) btrfsctl.o
	$(CC) $(CFLAGS) -o btrfsctl btrfsctl.o $(objects) $(LDFLAGS) $(LIBS)

btrfs-vol: $(objects) btrfs-vol.o
	$(CC) $(CFLAGS) -o btrfs-vol btrfs-vol.o $(objects) $(LDFLAGS) $(LIBS)

btrfs-show: $(objects) btrfs-show.o
	$(CC) $(CFLAGS) -o btrfs-show btrfs-show.o $(objects) $(LDFLAGS) $(LIBS)

btrfsck: $(objects) btrfsck.o
	$(CC) $(CFLAGS) -o btrfsck btrfsck.o $(objects) $(LDFLAGS) $(LIBS)

mkfs.btrfs: $(objects) mkfs.o
	$(CC) $(CFLAGS) -o mkfs.btrfs $(objects) mkfs.o $(LDFLAGS) $(LIBS)

btrfs-debug-tree: $(objects) debug-tree.o
	$(CC) $(CFLAGS) -o btrfs-debug-tree $(objects) debug-tree.o $(LDFLAGS) $(LIBS)

btrfs-zero-log: $(objects) btrfs-zero-log.o
	$(CC) $(CFLAGS) -o btrfs-zero-log $(objects) btrfs-zero-log.o $(LDFLAGS) $(LIBS)

btrfs-select-super: $(objects) btrfs-select-super.o
	$(CC) $(CFLAGS) -o btrfs-select-super $(objects) btrfs-select-super.o $(LDFLAGS) $(LIBS)

btrfstune: $(objects) btrfstune.o
	$(CC) $(CFLAGS) -o btrfstune $(objects) btrfstune.o $(LDFLAGS) $(LIBS)

btrfs-map-logical: $(objects) btrfs-map-logical.o
	$(CC) $(CFLAGS) -o btrfs-map-logical $(objects) btrfs-map-logical.o $(LDFLAGS) $(LIBS)

btrfs-corrupt-block: $(objects) btrfs-corrupt-block.o
	$(CC) $(CFLAGS) -o btrfs-corrupt-block $(objects) btrfs-corrupt-block.o $(LDFLAGS) $(LIBS)

btrfs-image: $(objects) btrfs-image.o
	$(CC) $(CFLAGS) -o btrfs-image $(objects) btrfs-image.o -lpthread -lz $(LDFLAGS) $(LIBS)

dir-test: $(objects) dir-test.o
	$(CC) $(CFLAGS) -o dir-test $(objects) dir-test.o $(LDFLAGS) $(LIBS)

quick-test: $(objects) quick-test.o
	$(CC) $(CFLAGS) -o quick-test $(objects) quick-test.o $(LDFLAGS) $(LIBS)

btrfs-convert: $(objects) convert.o
	$(CC) $(CFLAGS) -o btrfs-convert $(objects) convert.o -lext2fs -lcom_err $(LDFLAGS) $(LIBS)

ioctl-test: $(objects) ioctl-test.o
	$(CC) $(CFLAGS) -o ioctl-test $(objects) ioctl-test.o $(LDFLAGS) $(LIBS)

manpages:
	cd man; $(MAKE)

install-man:
	cd man; $(MAKE) install

clean :
	rm -f $(progs) cscope.out *.o .*.d btrfs-convert btrfs-image btrfs-select-super \
	      btrfs-zero-log btrfstune dir-test ioctl-test quick-test version.h
	cd man; $(MAKE) clean

install: $(progs) install-man
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(progs) $(DESTDIR)$(bindir)

-include .*.d
