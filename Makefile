CC=gcc
CFLAGS = -g -Wall -fno-strict-aliasing -Werror
objects = ctree.o disk-io.o radix-tree.o extent-tree.o print-tree.o \
	  root-tree.o dir-item.o hash.o file-item.o inode-item.o \
	  inode-map.o crc32c.o rbtree.o pending-extent.o \
#
CHECKFLAGS=-D__linux__ -Dlinux -D__STDC__ -Dunix -D__unix__ -Wbitwise \
		-Wuninitialized -Wshadow -Wundef
DEPFLAGS = -Wp,-MMD,$(@D)/.$(@F).d,-MT,$@

INSTALL= install
prefix ?= /usr/local
bindir = $(prefix)/bin

progs = btrfsctl btrfsck mkfs.btrfs debug-tree

# make C=1 to enable sparse
ifdef C
	check=sparse $(CHECKFLAGS)
else
	check=ls
endif

.c.o:
	$(check) $<
	$(CC) $(DEPFLAGS) $(CFLAGS) -c $<


all: $(progs)

btrfsctl: btrfsctl.o
	gcc $(CFLAGS) -o btrfsctl btrfsctl.o

btrfsck: $(objects) btrfsck.o bit-radix.o
	gcc $(CFLAGS) -o btrfsck btrfsck.o $(objects) bit-radix.o

mkfs.btrfs: $(objects) mkfs.o
	gcc $(CFLAGS) -o mkfs.btrfs $(objects) mkfs.o -luuid

debug-tree: $(objects) debug-tree.o
	gcc $(CFLAGS) -o debug-tree $(objects) debug-tree.o -luuid

dir-test: $(objects) dir-test.o
	gcc $(CFLAGS) -o dir-test $(objects) dir-test.o

quick-test: $(objects) quick-test.o
	gcc $(CFLAGS) -o quick-test $(objects) quick-test.o

clean :
	rm -f $(progs) cscope.out *.o .*.d

install: $(progs)
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(progs) $(DESTDIR)$(bindir)

-include .*.d
