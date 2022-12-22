Source repositories
===================

Since 2.6.29-rc1, Btrfs has been included in the mainline kernel.

Kernel module
-------------

The kernel.org git repository is not used for development, only for pull
requests that go to Linus and for linux-next integration:

* https://git.kernel.org/pub/scm/linux/kernel/git/kdave/linux.git -- pull requests, branch *for-next* gets pulled to the linux-next tree

The following git repositories are used for development and are updated with
patches from the mailing list:

* https://github.com/kdave/btrfs-devel
* https://gitlab.com/kdave/btrfs-devel

Branches are usually pushed to both repositories, either can be used.

There are:

* main queue with patches for next development cycle (branch name *misc-next*)
* queue with patches for current release cycle (the name has the version, e.g. *for-4.15* or *misc-4.15*).
* topic branches, e.g. from a patchset picked from mailing list
* snapshots of *for-next*, that contain all of the above (e.g. for-next-20200512)

Note that the branches get rebased.  The base point for patches depend on the
development phase.  See [[Developer%27s_FAQ#Development_schedule]].
Independent changes can be based on the *linus/master* branch, changes that
could depend on patches that have been added to one of the queues should use
that as a base.

btrfs-progs git repository
--------------------------

Official repositories
^^^^^^^^^^^^^^^^^^^^^

The sources of the userspace utilities can be obtained from these repositories:

* git://git.kernel.org/pub/scm/linux/kernel/git/kdave/btrfs-progs.git (
  http://git.kernel.org/?p=linux/kernel/git/kdave/btrfs-progs.git;a=summary)
  -- release repository, not for development

The **master** branch contains the latest released version and is never rebased.

Development git repositories:

* git://github.com/kdave/btrfs-progs.git (https://github.com/kdave/btrfs-progs)
* git://gitlab.com/kdave/btrfs-progs.git (https://gitlab.com/kdave/btrfs-progs)

For build dependencies and installation instructions please see
https://github.com/kdave/btrfs-progs/blob/master/INSTALL

Development branches
^^^^^^^^^^^^^^^^^^^^

The latest **development branch** is called **devel**. Contains patches that
are reviewed or tested and on the way to the next release. When a patch is
added to the branch, a mail notification is sent as a reply to the patch.

The git repositories on *kernel.org* are not used for development or
integration branches.

Note to GitHub users
^^^^^^^^^^^^^^^^^^^^

The pull requests will not be accepted directly, the preferred way is to send
patches to the mailing list instead. You can link to a branch in any git
repository if the mails do not make it to the mailing list or for convenience.

The development model of btrfs-progs shares a lot with the kernel model. The
github.com way is different in some ways. We, the upstream community, expect that
the patches meet some criteria (often lacking in github.com contributions):

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

        $ wget -O - '<nowiki>https://patchwork.kernel.org/patch/123456/mbox</nowiki>' | git am -

You may want to add *--reject*, or decide otherwise what to do with the patch.
