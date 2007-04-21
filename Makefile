CC=gcc
CFLAGS = -g -Wall -Werror
headers = radix-tree.h ctree.h disk-io.h kerncompat.h print-tree.h list.h \
	  transaction.h ioctl.h
objects = ctree.o disk-io.o radix-tree.o extent-tree.o print-tree.o \
	  root-tree.o dir-item.o hash.o file-item.o inode-item.o \
	  inode-map.o \
#
# if you don't have sparse installed, use ls instead
CHECKFLAGS=-D__linux__ -Dlinux -D__STDC__ -Dunix -D__unix__ -Wbitwise \
		-Wuninitialized -Wshadow -Wundef
check=sparse $(CHECKFLAGS)
#check=ls

.c.o:
	$(check) $<
	$(CC) $(CFLAGS) -c $<

all: tester debug-tree quick-test dir-test mkfs.btrfs \
	btrfsctl btrfsck

btrfsctl: ioctl.h btrfsctl.o $(headers)
	gcc $(CFLAGS) -o btrfsctl btrfsctl.o

btrfsck: btrfsck.o $(headers) bit-radix.o
	gcc $(CFLAGS) -o btrfsck btrfsck.o $(objects) bit-radix.o

mkfs.btrfs: $(objects) mkfs.o $(headers)
	gcc $(CFLAGS) -o mkfs.btrfs $(objects) mkfs.o -luuid

bit-radix-test: $(objects) bit-radix.o $(headers)
	gcc $(CFLAGS) -o bit-radix-test $(objects) bit-radix.o

debug-tree: $(objects) debug-tree.o $(headers)
	gcc $(CFLAGS) -o debug-tree $(objects) debug-tree.o -luuid

tester: $(objects) random-test.o $(headers)
	gcc $(CFLAGS) -o tester $(objects) random-test.o

dir-test: $(objects) dir-test.o $(headers)
	gcc $(CFLAGS) -o dir-test $(objects) dir-test.o
quick-test: $(objects) quick-test.o $(headers)
	gcc $(CFLAGS) -o quick-test $(objects) quick-test.o

clean :
	rm debug-tree mkfs.btrfs btrfsctl btrfsck *.o


