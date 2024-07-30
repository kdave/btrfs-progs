Source repositories
===================

Since 2.6.29-rc1, Btrfs has been included in the mainline kernel.

Kernel module
-------------

The kernel.org git repository is not used for development, only for pull
requests that go to Linus and for `linux-next <https://git.kernel.org/pub/scm/linux/kernel/git/next/linux-next.git>`__
integration:

* https://git.kernel.org/pub/scm/linux/kernel/git/kdave/linux.git -- pull request source
* branch `for-next <https://git.kernel.org/pub/scm/linux/kernel/git/kdave/linux.git/log/?h=for-next>`__
  gets pulled to the *linux-next* tree, is rebased and contains base
  development branches and topic branches
* branch `next-fixes <https://git.kernel.org/pub/scm/linux/kernel/git/kdave/linux.git/log/?h=next-fixes>`__
  has fixes for the next upcoming *rcN* and is usually turned into a pull request

The following git repository is used for group development and is updated with
patches from the mailing list:

* https://github.com/btrfs/linux

The main branch with patches is `for-next <https://github.com/btrfs/linux/tree/for-next>`__ .
Note that it gets rebased or updated (fixed typos, added Reviewed-by
tags etc).  The base point for patches depend on the development phase.  See
:ref:`development schedule<devfaq-development-schedule>`.  Independent changes
can be based on the *linus/master* branch, changes that could depend on patches
that have been added to one of the queues should use that as a base.

btrfs-progs git repository
--------------------------

Release repositories
^^^^^^^^^^^^^^^^^^^^

The sources of the userspace utilities can be obtained from these repositories:

* git://git.kernel.org/pub/scm/linux/kernel/git/kdave/btrfs-progs.git
  (`<http://git.kernel.org/?p=linux/kernel/git/kdave/btrfs-progs.git;a=summary>`__)
  - release repository, not for development

The **master** branch contains the latest released version is never rebased and
updated after a release.

Development repositories
^^^^^^^^^^^^^^^^^^^^^^^^

* git://github.com/kdave/btrfs-progs.git (`<https://github.com/kdave/btrfs-progs>`__)
* git://gitlab.com/kdave/btrfs-progs.git (`<https://gitlab.com/kdave/btrfs-progs>`__)

For build dependencies and installation instructions please see
https://github.com/kdave/btrfs-progs/blob/master/INSTALL

Development branches
^^^^^^^^^^^^^^^^^^^^

The latest **development branch** is called **devel**. Contains patches that
are reviewed or tested and on the way to the next release. When a patch is
added to the branch, a mail notification is sent as a reply to the patch.

The git repositories on *kernel.org* are not used for development or
integration branches.

GitHub development
^^^^^^^^^^^^^^^^^^

Pull requests are accepted for contributions, and slightly more preferred as
they get tested by the CI (Github actions).  Patches to the mailing are also
accepted but not mandatory. You can link to a branch in any git repository if
the mails do not make it to the mailing list or for convenience.

The development model of btrfs-progs has moved from kernel model to
github and is less strict about some things. :doc:`dev/GithubReviewWorkflow`. 

It is still desired to write good changelogs:

* proper **subject line**: e.g. prefix with *btrfs-progs: subpart, ...* ,
  descriptive yet not too long
* proper **changelog**: the changelogs are often missing or lacking
  explanation *why* the change was made, or *how* is something broken,
  *what* are user-visible effects of the bug or the fix, *how* does an
  improvement help or the intended *usecase*
* the **Signed-off-by** line: this document who authored the change, you can
  read more about the *The Developer's Certificate of Origin*
  `here (chapter 11) <https://www.kernel.org/doc/Documentation/SubmittingPatches>`_]
* **one logical change** per patch: e.g. not mixing bug fixes, cleanups,
  features etc., sometimes it's not clear and will be usually pointed out
  during reviews
* references to reports, issues, pull requests

Pull requests allow to update commits, fixups are possible and recommended.


Administration and support tools
--------------------------------

There is a separate repository of useful scripts for common administrative
tasks on btrfs. This is at:

https://github.com/kdave/btrfsmaintenance/

Patches sent to mailing list
----------------------------

A convenient interface to get an overview of patches and the related mail
discussions can be found at
https://patchwork.kernel.org/project/linux-btrfs/list/ .

It is possible to directly apply a patch by pasting the *mbox* link from the
patch page to the command:

.. code-block:: bash

        $ wget -O - 'https://patchwork.kernel.org/patch/123456/mbox' | git am -

You may want to add *--reject*, or decide otherwise what to do with the patch.
