CC=gcc
AM_CFLAGS = -Wall -D_FILE_OFFSET_BITS=64 -D_FORTIFY_SOURCE=2
CFLAGS = -g -Werror -Os
objects = ctree.o disk-io.o radix-tree.o extent-tree.o print-tree.o \
	  root-tree.o dir-item.o file-item.o inode-item.o \
	  inode-map.o crc32c.o rbtree.o extent-cache.o extent_io.o \
	  volumes.o utils.o

#
CHECKFLAGS=-D__linux__ -Dlinux -D__STDC__ -Dunix -D__unix__ -Wbitwise \
		-Wuninitialized -Wshadow -Wundef
DEPFLAGS = -Wp,-MMD,$(@D)/.$(@F).d,-MT,$@

INSTALL= install
prefix ?= /usr/local
bindir = $(prefix)/bin
LIBS=-luuid

progs = btrfsctl mkfs.btrfs btrfs-debug-tree btrfs-show btrfs-vol btrfsck

# make C=1 to enable sparse
ifdef C
	check=sparse $(CHECKFLAGS)
else
	check=ls
endif

.c.o:
	$(check) $<
	$(CC) $(DEPFLAGS) $(AM_CFLAGS) $(CFLAGS) -c $<


all: version $(progs) manpages

version:
	bash version.sh

btrfsctl: $(objects) btrfsctl.o
	gcc $(CFLAGS) -o btrfsctl btrfsctl.o $(objects) $(LDFLAGS) $(LIBS)

btrfs-vol: $(objects) btrfs-vol.o
	gcc $(CFLAGS) -o btrfs-vol btrfs-vol.o $(objects) $(LDFLAGS) $(LIBS)

btrfs-show: $(objects) btrfs-show.o
	gcc $(CFLAGS) -o btrfs-show btrfs-show.o $(objects) $(LDFLAGS) $(LIBS)

btrfsck: $(objects) btrfsck.o
	gcc $(CFLAGS) -o btrfsck btrfsck.o $(objects) $(LDFLAGS) $(LIBS)

mkfs.btrfs: $(objects) mkfs.o
	gcc $(CFLAGS) -o mkfs.btrfs $(objects) mkfs.o $(LDFLAGS) $(LIBS)

btrfs-debug-tree: $(objects) debug-tree.o
	gcc $(CFLAGS) -o btrfs-debug-tree $(objects) debug-tree.o $(LDFLAGS) $(LIBS)

btrfstune: $(objects) btrfstune.o
	gcc $(CFLAGS) -o btrfstune $(objects) btrfstune.o $(LDFLAGS) $(LIBS)

btrfs-image: $(objects) btrfs-image.o
	gcc $(CFLAGS) -o btrfs-image $(objects) btrfs-image.o -lpthread -lz $(LDFLAGS) $(LIBS)

dir-test: $(objects) dir-test.o
	gcc $(CFLAGS) -o dir-test $(objects) dir-test.o $(LDFLAGS) $(LIBS)

quick-test: $(objects) quick-test.o
	gcc $(CFLAGS) -o quick-test $(objects) quick-test.o $(LDFLAGS) $(LIBS)

convert: $(objects) convert.o
	gcc $(CFLAGS) -o btrfs-convert $(objects) convert.o -lext2fs $(LDFLAGS) $(LIBS)

manpages:
	cd man; make

install-man:
	cd man; make install

clean :
	rm -f $(progs) cscope.out *.o .*.d btrfs-convert
	cd man; make clean

install: $(progs) install-man
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(progs) $(DESTDIR)$(bindir)
	if [ -e btrfs-convert ]; then $(INSTALL) btrfs-convert $(DESTDIR)$(bindir); fi

-include .*.d
