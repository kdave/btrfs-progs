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
from docutils import nodes
from docutils.utils import unescape
from docutils.parsers.rst import Directive
from sphinx.util.nodes import split_explicit_title, set_source_info

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

html_theme_options = {
    'navigation_with_keys': True
}

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
    ('btrfs-ioctl', 'btrfs-ioctl', 'documentation about btrfs ioctls', '', 2),
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

extensions = [ 'sphinx_rtd_theme' ]

# Cross reference with document and label
# Syntax: :docref`Title <rawdocname:label>`
# Backends: html, man, others
# - title is mandatory, for manual page backend will append "in rawdocname" if it's in another document
# - label is not yet validated, can be duplicate in more documents
# - rawdocname is without extension
def role_docref(name, rawtext, text, lineno, inliner, options={}, content=[]):
    env = inliner.document.settings.env
    text = unescape(text)
    has_explicit_title, title, target = split_explicit_title(text)
    if not has_explicit_title:
        msg = inliner.reporter.error(f"docref requires title: {rawtext}", line=lineno)
        prb = inliner.problematic(rawtext, rawtext, msg)
        return [prb], [msg]

    try:
        docname, label = target.split(':', 1)
    except ValueError:
        msg = inliner.reporter.error(f"invalid docref syntax {target}", line=lineno)
        prb = inliner.problematic(rawtext, rawtext, msg)
        return [prb], [msg]

    # inliner.reporter.warning(f"DBG: docname={docname} label={label} env.docname={env.docname} title={title}")

    # Validate doc
    if docname not in env.found_docs:
        docs = list(env.found_docs)
        msg = inliner.reporter.error(f"document not found {docname} (%s" % (docs), line=lineno)
        prb = inliner.problematic(rawtext, rawtext, msg)
        return [prb], [msg]

    # TODO: validate label

    suffix = ''
    if env.app.builder.name == 'html':
        suffix = '.html'
    elif env.app.builder.name == 'man':
        suffix = '//'

    titlesuffix = ''
    if docname != env.docname:
        titlesuffix = f" (in {docname})"

    try:
        ref_node = nodes.reference(rawtext, title + titlesuffix,
                                   refuri=f"{docname}{suffix}#{label}", **options)
    except ValueError:
        msg = inliner.reporter.error('invalid cross reference %r' % text, line=lineno)
        prb = inliner.problematic(rawtext, rawtext, msg)
        return [prb], [msg]
    return [ref_node], []

# Directive to define a label that can appear multiple time (e.g. from an included
# document), no warnings
# Must be used in connection with :docref: to link to the containing rather than
# included document
# Syntax: .. duplabel:: label-name
# Backends: all
class DupLabelDirective(Directive):
    required_arguments = 1

    def run(self):
        label = self.arguments[0]
        target_node = nodes.target('', '', ids=[label])
        env = self.state.document.settings.env
        line_number = self.state.document.current_line
        env.domaindata['std']['labels'][label] = (env.docname, label, line_number)
        set_source_info(self, target_node)
        return [target_node]

# Manual page reference or link to man7.org
# Syntax: :manref:`page(1)`
# Backends: html, man
# - format is strict
# - html link target is not validated
def role_manref(name, rawtext, text, lineno, inliner, options={}, content=[]):
    env = inliner.document.settings.env
    name, number = text.split('(', 1)
    number = number.split(')')[0]

    try:
        ref_node = nodes.reference(text, f"{name}({number})",
                       refuri=f"https://man7.org/linux/man-pages/man{number}/{name}.{number}.html")

    except Exception as e:
        inliner.reporter.warning(f"Error creating manref role: {str(e)}", line=lineno)
        return [], []
    return [ref_node], []

def setup(app):
    app.add_role('docref', role_docref)
    app.add_role('manref', role_manref)
    app.add_directive('duplabel', DupLabelDirective)
