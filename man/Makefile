GZIP=gzip
INSTALL= install

prefix ?= /usr/local
bindir = $(prefix)/bin
mandir = $(prefix)/man
man8dir = $(mandir)/man8

MANPAGES = mkfs.btrfs.8.gz btrfsctl.8.gz btrfsck.8.gz btrfs-image.8.gz \
	   btrfs-show.8.gz btrfs.8.gz

all: $(MANPAGES)

mkfs.btrfs.8.gz: mkfs.btrfs.8.in
	$(GZIP) -n -c mkfs.btrfs.8.in > mkfs.btrfs.8.gz

btrfs.8.gz: btrfs.8.in
	$(GZIP) -n -c btrfs.8.in > btrfs.8.gz

btrfsctl.8.gz: btrfsctl.8.in
	$(GZIP) -n -c btrfsctl.8.in > btrfsctl.8.gz

btrfsck.8.gz: btrfsck.8.in
	$(GZIP) -n -c btrfsck.8.in > btrfsck.8.gz

btrfs-image.8.gz: btrfs-image.8.in
	$(GZIP) -n -c btrfs-image.8.in > btrfs-image.8.gz

btrfs-show.8.gz: btrfs-show.8.in
	$(GZIP) -n -c btrfs-show.8.in > btrfs-show.8.gz

clean :
	rm -f $(MANPAGES)

install: $(MANPAGES)
	$(INSTALL) -m755 -d $(DESTDIR)$(man8dir)
	$(INSTALL) -m 644 $(MANPAGES) $(DESTDIR)$(man8dir)
