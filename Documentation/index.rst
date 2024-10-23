.. BTRFS documentation master file

Welcome to BTRFS documentation!
===============================

BTRFS is a modern copy on write (COW) filesystem for Linux aimed at
implementing advanced features while also focusing on fault tolerance, repair
and easy administration. You can read more about the features in the
:doc:`introduction<Introduction>` or choose from the pages below. Documentation
for command line tools :doc:`btrfs`, :doc:`mkfs.btrfs` and others
is in the :doc:`manual pages<man-index>`.

.. raw:: html

   <table><tr><td class="main-table-col">

.. toctree::
   :maxdepth: 1
   :caption: Overview

   Introduction
   Status
   man-index
   Administration
   Hardware
   Feature-by-version
   Kernel-by-version
   CHANGES
   Contributors
   Glossary
   INSTALL
   Source-repositories
   Interoperability

.. raw:: html

   </td><td class="main-table-col">

.. toctree::
   :maxdepth: 1
   :caption: Features

   Common-features
   Custom-ioctls
   Auto-repair
   Balance
   Compression
   Checksumming
   Convert
   Deduplication
   Defragmentation
   Inline-files
   Qgroups
   Reflink
   Resize
   Scrub
   Seeding-device
   Send-receive
   Subpage
   Subvolumes
   Swapfile
   Tree-checker
   Trim
   Volume-management
   Zoned-mode

.. raw:: html

   </td><td class="main-table-col">

.. toctree::
   :maxdepth: 1
   :caption: Developer documentation

   dev/Development-notes
   dev/Developer-s-FAQ
   DocConventions
   dev/Experimental
   dev/dev-btrfs-design
   dev/dev-btrees
   dev/On-disk-format
   dev/dev-send-stream
   dev/dev-json
   dev/dev-internal-apis
   dev/ReleaseChecklist
   dev/GithubReviewWorkflow
   btrfs-ioctl


.. raw:: html

   </td></tr></table>

Need help?
----------

Assistance is available from the `#btrfs channel on Libera Chat <https://web.libera.chat/#btrfs>`_ or the `linux-btrfs mailing list <https://subspace.kernel.org/vger.kernel.org.html>`_. Issues with the userspace btrfs tools can be reported to the `btrfs-progs issue tracker on GitHub <https://github.com/kdave/btrfs-progs/issues>`_.

.. raw:: html

   <hr />

This documentation is still work in progress, not everything from the original
wiki https://btrfs.wiki.kernel.org has been moved here. Below are starting points
for missing contents.

.. toctree::
   :maxdepth: 1
   :caption: TODO

   Quick-start
   trouble-index
