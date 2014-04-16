# Guard against environment variables
MAN8_TXT =

# Top level commands
MAN8_TXT += btrfs.txt
MAN8_TXT += btrfs-convert.txt
MAN8_TXT += btrfs-debug-tree.txt
MAN8_TXT += btrfs-find-root.txt
MAN8_TXT += btrfs-image.txt
MAN8_TXT += btrfs-map-logical.txt
MAN8_TXT += btrfs-show-super.txt
MAN8_TXT += btrfstune.txt
MAN8_TXT += btrfs-zero-log.txt
MAN8_TXT += fsck.btrfs.txt
MAN8_TXT += mkfs.btrfs.txt

# Sub commands for btrfs
MAN8_TXT += btrfs-subvolume.txt
MAN8_TXT += btrfs-filesystem.txt
MAN8_TXT += btrfs-balance.txt
MAN8_TXT += btrfs-device.txt
MAN8_TXT += btrfs-scrub.txt
MAN8_TXT += btrfs-check.txt
MAN8_TXT += btrfs-rescue.txt
MAN8_TXT += btrfs-inspect-internal.txt
MAN8_TXT += btrfs-send.txt
MAN8_TXT += btrfs-receive.txt
MAN8_TXT += btrfs-quota.txt
MAN8_TXT += btrfs-qgroup.txt
MAN8_TXT += btrfs-replace.txt
MAN8_TXT += btrfs-restore.txt
MAN8_TXT += btrfs-property.txt

MAN_TXT = $(MAN8_TXT)
MAN_XML = $(patsubst %.txt,%.xml,$(MAN_TXT))
DOC_MAN8 = $(patsubst %.txt,%.8,$(MAN8_TXT))
GZ_MAN8 = $(patsubst %.txt,%.8.gz,$(MAN8_TXT))

mandir ?= $(prefix)/share/man
man8dir = $(mandir)/man8

ASCIIDOC = asciidoc
ASCIIDOC_EXTRA =
MANPAGE_XSL = manpage-normal.xsl
XMLTO = xmlto
XMLTO_EXTRA =
XMLTO_EXTRA = -m manpage-bold-literal.xsl
GZIP = gzip
INSTALL ?= install
RM ?= rm -f
LNS ?= ln -sf
BTRFS_VERSION = $(shell sed -n 's/.*BTRFS_BUILD_VERSION "Btrfs \(.*\)"/\1/p'\
		  ../version.h)

ifneq ($(findstring $(MAKEFLAGS),s),s)
ifndef V
	QUIET_ASCIIDOC	= @echo '   ' ASCIIDOC $@;
	QUIET_XMLTO	= @echo '   ' XMLTO $@;
	QUIET_GZIP	= @echo '   ' GZIP $@;
	QUIET_STDERR	= 2> /dev/null
	QUIET_SUBDIR0	= +@subdir=
	QUIET_SUBDIR1	= ;$(NO_SUBDIR) echo '   ' SUBDIR $$subdir; \
			  $(MAKE) $(PRINT_DIR) -C $$subdir
	export V
endif
endif

all: man
man: man8
man8: $(GZ_MAN8)

install: install-man

install-man: man
	$(INSTALL) -d -m 755 $(DESTDIR)$(man8dir)
	$(INSTALL) -m 644 $(GZ_MAN8) $(DESTDIR)$(man8dir)
	$(LNS) btrfs-check.8.gz $(DESTDIR)$(man8dir)

clean:
	$(RM) *.xml *.xml+ *.8 *.8.gz

%.8.gz : %.8
	$(QUIET_GZIP)$(GZIP) -n -c $< > $@

%.8 : %.xml 
	$(QUIET_XMLTO)$(RM) $@ && \
	$(XMLTO) -m $(MANPAGE_XSL) $(XMLTO_EXTRA) man $<

%.xml : %.txt asciidoc.conf
	$(QUIET_ASCIIDOC)$(RM) $@+ $@ && \
	$(ASCIIDOC) -b docbook -d manpage -f asciidoc.conf \
		$(ASCIIDOC_EXTRA) -abtrfs_version=$(BTRFS_VERSION) \
		-o $@+ $< && \
	mv $@+ $@
