GZIPCMD=gzip
INSTALL= install

prefix ?= /usr/local
bindir = $(prefix)/bin
mandir = $(prefix)/man
man8dir = $(mandir)/man8

# clear out all suffixes
.SUFFIXES:
# list only those we use
.SUFFIXES: .in .gz

MANPAGES = mkfs.btrfs.8.gz btrfsck.8.gz btrfs-image.8.gz btrfs.8.gz \
	   btrfs-debug-tree.8.gz btrfs-show-super.8.gz btrfs-find-root.8.gz \
	   btrfs-convert.8.gz btrfstune.8.gz btrfs-zero-log.8.gz btrfs-map-logical.8.gz \
	   fsck.btrfs.8.gz
INFILES = ${MANPAGES:.in=.gz}

all: $(MANPAGES)

.in.gz :
	@echo "    [MAN]    $@"
	$(Q)$(GZIPCMD) -n -c $< > $@

clean :
	$(Q)rm -f $(MANPAGES)

install: $(MANPAGES)
	$(INSTALL) -m755 -d $(DESTDIR)$(man8dir)
	$(INSTALL) -m 644 $(MANPAGES) $(DESTDIR)$(man8dir)
