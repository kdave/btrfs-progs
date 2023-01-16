# Configuration file for the Sphinx documentation builder.
#
# This file only contains a selection of the most common options. For a full
# list see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

# -- Path setup --------------------------------------------------------------

# If extensions (or modules to document with autodoc) are in another directory,
# add these directories to sys.path here. If the directory is relative to the
# documentation root, use os.path.abspath to make it absolute, like shown here.
#
# import os
# import sys
# sys.path.insert(0, os.path.abspath('.'))

import pathlib

# -- Project information -----------------------------------------------------
project = 'BTRFS'

version = pathlib.Path("../VERSION").read_text().strip('v\n')

# Add any paths that contain templates here, relative to this directory.
templates_path = ['_templates']

# List of patterns, relative to source directory, that match files and
# directories to ignore when looking for source files.
# This pattern also affects html_static_path and html_extra_path.
exclude_patterns = ['_build']

# The theme to use for HTML and HTML Help pages.  See the documentation for
# a list of builtin themes.
html_theme = 'sphinx_rtd_theme'

# Add any paths that contain custom static files (such as style sheets) here,
# relative to this directory. They are copied after the builtin static files,
# so a file named "default.css" will overwrite the builtin "default.css".
html_static_path = ['_static']

# Disable em-dash translation to a single character as we use that for long
# command line options and this does not render in a copy & paste friendly way
# in html
smartquotes_action = 'qe'

man_pages = [
    # Source file, page name, description, authors, section
    ('btrfs-select-super', 'btrfs-select-super', 'overwrite primary superblock with a backup copy', '', 8),
    ('btrfstune', 'btrfstune', 'tune various filesystem parameters', '', 8),
    ('fsck.btrfs', 'fsck.btrfs', 'do nothing, successfully', '', 8),
    ('btrfs-send', 'btrfs-send', 'generate a stream of changes between two subvolume snapshots', '', 8),
    ('btrfs-scrub', 'btrfs-scrub', 'scrub btrfs filesystem, verify block checksums', '', 8),
    ('btrfs-restore', 'btrfs-restore', 'try to restore files from a damaged filesystem image', '', 8),
    ('btrfs-rescue', 'btrfs-rescue', 'recover a damaged btrfs filesystem', '', 8),
    ('btrfs-replace', 'btrfs-replace', 'replace devices managed by btrfs with other device', '', 8),
    ('btrfs-receive', 'btrfs-receive', 'receive subvolumes from send stream', '', 8),
    ('btrfs-quota', 'btrfs-quota', 'control the global quota status of a btrfs filesystem', '', 8),
    ('btrfs-qgroup', 'btrfs-qgroup', 'control the quota group of a btrfs filesystem', '', 8),
    ('btrfs-property', 'btrfs-property', 'get/set/list properties for given filesystem object', '', 8),
    ('btrfs-inspect-internal', 'btrfs-inspect-internal', 'query various internal information', '', 8),
    ('btrfs-image', 'btrfs-image', 'create/restore an image of the filesystem', '', 8),
    ('btrfs-find-root', 'btrfs-find-root', 'filter to find btrfs root', '', 8),
    ('btrfs-filesystem', 'btrfs-filesystem', 'command group that primarily does work on the whole filesystems', '', 8),
    ('btrfs-device', 'btrfs-device', 'manage devices of btrfs filesystems', '', 8),
    ('btrfs-convert', 'btrfs-convert', 'convert from ext2/3/4 or reiserfs filesystem to btrfs in-place', '', 8),
    ('btrfs-check', 'btrfs-check', 'check or repair a btrfs filesystem', '', 8),
    ('btrfs-balance', 'btrfs-balance', 'balance block groups on a btrfs filesystem', '', 8),
    ('btrfs-subvolume', 'btrfs-subvolume', 'manage btrfs subvolumes', '', 8),
    ('btrfs-map-logical', 'btrfs-map-logical', 'map btrfs logical extent to physical extent', '', 8),
    ('btrfs', 'btrfs', 'a toolbox to manage btrfs filesystems', '', 8),
    ('mkfs.btrfs', 'mkfs.btrfs', 'create a btrfs filesystem', '', 8),
    ('btrfs-man5', 'btrfs', 'topics about the BTRFS filesystem (mount options, supported file attributes and other)', '', 5),
]
