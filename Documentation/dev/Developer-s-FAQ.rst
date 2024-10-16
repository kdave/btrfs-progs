Developer's FAQ
===============

Contributor's FAQ
-----------------

The term *contributor* is broader and does not only mean contribution of code.
The documentation is a significant part and this is where non-technical people
add value. The user's POV brings different questions than the developers' and
explaining things in human language is a good thing.

Sending patches to documentation follows the same practices as for the code.

The documentation referred here is of the userspace tools (btrfs-progs), the
manual pages or the documentation that's part of the tool help strings.

The fixes range from rewording unclear sections, fixing formatting, spelling,
or adding more examples.

Documentation patches have high chance of getting merged and released quickly.

Developer's FAQ
---------------

By the term *developer* is meant somebody who's working on the code.

This section assumes basics of working with *git*, sending patches via mail and
aims to cover the current practices.

Patch tags
~~~~~~~~~~

The practice of tagging patches in linux kernel community is documented in
https://www.kernel.org/doc/html/latest/process/submitting-patches.html, we'll
highlight the most frequently used tags and their expected meaning. This only
briefly mentions the commonly used tags. You're encouraged to read the whole
document and get familiar with it.

Signed-off-by:
^^^^^^^^^^^^^^

This tag may appear multiple times, the first one denotes the patch author.
(The common abbreviation in free text is S-O-B or just sob line.) The patch
author is also recorded in git log history.

Then, each maintainer that processed the patch adds his sob line.

*Reference:* `Section of SubmittingPatches <https://docs.kernel.org/process/submitting-patches.html#sign-your-work-the-developer-s-certificate-of-origin>`__.

**Do**: Always send a patch with at least one such line with your name and email.
If more people contributed to the patch, add their names and addresses too.

**Don't**: Add a sob line under patch that you have no authoring relation to, e.g.
as a reply to the mailinglist after you've reviewed a patch. See below.

Reviewed-by:
^^^^^^^^^^^^

The patch has been reviewed and the signed person is putting his hand into
fire. If there's a bug found in this patch, the person is usually a good
candidate for a CC: of the bugreport.

*Reference:* `Section of SubmittingPatches <https://docs.kernel.org/process/submitting-patches.html#using-reported-by-tested-by-reviewed-by-suggested-by-and-fixes>`__.

**Do**: talk to the maintainer if he forgot to add this tag to the final patch.
Reviews do take time and the patches land in various branches early after
they're sent to the mailingslist for testing, but the reviews are always
welcome.

**Do**: collect the Reviewed-by tags for patches that get resent unchanged e.g.
within a larger patch series

Acked-by:
^^^^^^^^^

A more lightweight form of *Reviewed-by*, acknowledging that the patch is going
the right direction, but that the person has not done a deeper examination of
the patch. Asking for an ACK can be expressed by a *CC:* tag in the patch.

Tested-by:
^^^^^^^^^^

Indicates that the patch has been successfully tested in some environment,
usually follows a proposed fix and closes the feedback loop.

*Reference:* `Section of SubmittingPatches <https://docs.kernel.org/process/submitting-patches.html#using-reported-by-tested-by-reviewed-by-suggested-by-and-fixes>`_.

**Do**: or rather you're encouraged to add this tag to a patch that you've
tested.

CC:
^^^

Add this tag to the patch if you feel that the person should be aware of the
patch.

Ordering
^^^^^^^^

The order of the tags can track the flow of the patches through various trees,
namely the Signed-off-by tag. Ordering of the other tags is not strict so you
can find patches with randomly mixed tags. A common practice we find kind of
useful is to sort them how things happened. It would be good to use that,
namely the references to stable trees and original reports.

-  how it happened

   #. (optional) Bugzilla:
   #. (optional) Link:
   #. **Reported-by:**

-  where it should be backported, relevant references

   #. **Fixes:**
   #. **CC:** stable\@vger.kernel.org # 4.4+

-  other tags

   #. **CC:**
   #. **Suggested-by:**

-  quality control by non-authors

   #. **Reviewed-by:**
   #. **Tested-by:**

-  author(s)

   #. **Signed-off-by:**

-  maintainer(s)

   #. **Reviewed-by:**
   #. **Signed-off-by:**

Patch flow
~~~~~~~~~~

Simple patch
^^^^^^^^^^^^

#. developer works on the patch, self-reviews, tests, adds the formal tags,
   writes changelog
#. patch lands in the mailinglist
#. patch is commented, reviewed

   -  several iterations of updates may follow

#. maintainer adds the patch into a branch
#. when the right time comes, a branch with selected patches is pushed up the
   merge chain
#. a release milestone that contains the patch is released, everybody is happy

Controversial changes
^^^^^^^^^^^^^^^^^^^^^

This happens, not every patch gets merged. In the worst case there are not even
any comments under the patch and it's silently ignored. This depends on many
factors, most notably \*cough*time*cough*. Examining potential drawbacks or
foreseeing disasters is not an easy job.

Let's be more positive, you manage to attract the attention of some developer
and he says, he does not like the approach of the patch(es).  Better than
nothing, isn't it? Depending on the feedback, try to understand the objections
and try to find a solution or insist on your approach but possibly back it by
good arguments (performance gain, expected use case) or a better explanation
*why* the change is needed.

Repeated submissions
^^^^^^^^^^^^^^^^^^^^

If you got feedback for a patch that pointed out changes that should be done
before the patch can be merged, please do apply the changes or give a reason
why they're wrong or not needed. (You can try to pinkie-swear to implement them
later, but do not try this too often.)

For the next iteration, add a short description of the changes made, under the
first **---** (triple dash) marker in the patch. For example (see also Example
3):

.. code-block:: none

   ---
   V3: renamed variable
   V2: fixed typo

Keep all previous changelogs. Larger patchsets should contain the incremental
changelogs in the cover letter.

Patch completeness, RFC
~~~~~~~~~~~~~~~~~~~~~~~

A patch does not necessarily have to implement the whole feature or idea. You
can send an early version, use a *[RFC]* string somewhere in the subject. This
means *request for comments*. Be prepared to get comments.

Please describe the level of completeness, e.g. what tests it does or does not
pass or what type of use cases is not yet implemented. The purpose is to get
feedback from other developers about the direction or implementation approach.
This may save you hours of coding.

Patchsets
~~~~~~~~~

Related patches, patch dependency
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Group the patches by feature or by topic. Implementing a particular feature may
need to prepare other parts of the code for the main patch.  Applying the
patches out of order will not succeed, so it's pointless to send them as
unrelated and separate mails. The git tool is helpful here, see
*git-format-patch*.

An example of grouping by topic is cleanups, or small bugfixes that are quite
independent but it would be better to processes them in one go.

Sometimes a patch from a series is self-contained enough that it might get
applied ahead of the whole series. You may also submit it separately as this
will decrease the work needed to keep the patch series up to date with the
moving development base.

**Do:** make sure that each patch compiles and does not deliberately introduce
a bug, this is a good practice that makes *bisecting* go smooth

**Do:** send the cover letter (i.e. the non-patch mail) with brief description
of the series, this is a place where feedback to the whole patchset will be
sent rather than comments to the individual patches. To generate the series
with cover letter use *git format-patch --cover-letter --thread*.

Good practices, contribution hints
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

-  if you feel that you understand some area of code enough to stick your
   *Reviewed-by* to submitted patches, please do. Even for small patches.
-  don't hesitate to be vocal if you see that a wrong patch has been committed
-  be patient if your patch is not accepted immediately, try to send a gentle
   ping if there's a significant time without any action
-  if you want to start contributing but are not sure about how to do that,
   lurk in the mailingist or on the IRC channel
-  every patch should implement one thing -- this is vaguely defined, you may
   receive comments about patch splitting or merging with other
-  every patch must be compilable when applied, possibly with all related
   CONFIG\_ variable values
-  send a new patch as a new mail, not within another thread, it might get
   missed
-  use *git-format-patch* and *git-send-email*

Sample patches
~~~~~~~~~~~~~~

There are some formalities expected, like subject line formatting, or the tags.
Although you may find them annoying at first, they help to classify the patches
among the rest of mails.

Subject:
^^^^^^^^

| For kernel patches add the prefix **btrfs:**
| for userspace tools add **btrfs-progs:**

Example 1
^^^^^^^^^

.. code-block:: none

   From: John Doe <john@doe.org>
   Subject: [PATCH] btrfs: merge common code into a helper

   The code for creating a new tree is open-coded in a few places, add a helper
   and remove the duplicate code.

   Signed-off-by: John Doe <john@doe.org>
   ---
   fs/btrfs/volumes.c |    5 +++--
   1 files changed, 3 insertions(+), 2 deletions(-)
   diff --git a/fs/btrfs/volumes.c b/fs/btrfs/volumes.c
   index e138af710de2..3f0cc12ec488 100644
   --- a/fs/btrfs/volumes.c
   +++ b/fs/btrfs/volumes.c
   (rest of the patch)

Example 2
^^^^^^^^^

.. code-block:: none

   From: Jane Doe <jane@doe.org>
   Subject: [PATCH] btrfs-progs: enhance documentation of balance

   Add examples of typical balance use, common problems and how to resolve them.

   Signed-off-by: Jane Doe <jane@doe.org>
   ---
   Documentation/btrfs-balance.txt |    20 +++++++++++++++++++-
   1 files changed, 3 insertions(+), 2 deletions(-)
   diff --git a/Documentation/btrfs-balance.txt b/Documentation/btrfs-balance.txt
   index e138af710de2..3f0cc12ec488 100644
   --- a/Documentation/btrfs-balance.txt
   +++ b/Documentation/btrfs-balance.txt
   (rest of the patch)

Example 3
^^^^^^^^^

.. code-block:: none

   From: John Doe <john@doe.org>
   Subject: [PATCH v3] btrfs: merge common code into a helper

   The code for creating a new tree is open-coded in a few places, add a helper
   and remove the duplicate code.

   Signed-off-by: John Doe <john@doe.org>
   ---
   V3: add helper prototype into header file
   V2: found one more open-coded instance

   fs/btrfs/ctree.h   |    1 +
   fs/btrfs/volumes.c |    5 +++--
   2 files changed, 4 insertions(+), 2 deletions(-)
   diff --git a/fs/btrfs/volumes.c b/fs/btrfs/volumes.c
   index e138af710de2..3f0cc12ec488 100644
   --- a/fs/btrfs/volumes.c
   +++ b/fs/btrfs/volumes.c
   (rest of the patch)

Pull requests vs patches to mailinglist
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

By default, all patches should be sent as mails to the mailinglist. The
discussions or reviews happen there. Putting a patch series to a git branch may
be convenient, but does not mean the exact unchanged branch will be pulled.

There are some criteria that have to be met before this happens. The patches
should meet/have:

-  no coding style violations
-  good quality of implementation, should not exhibit trivial mistakes, lack of
   comments
-  unspecified number of other things that usually get pointed out in review
   comments

   -  this knowledge can be demonstrated by doing reviews of other developers'
      patches
   -  doing reviews of other developers' patches is strongly recommended

-  good changelogs and subject lines
-  the base point of the git branch is well-defined (i.e. a stable release point
   or last development point, that will not get rebased)

The third point is vague, mostly refers to preferred coding patterns that we
discover and use over time. This may also explain why the pull-based workflow
is not used often. Both parties, developers and maintainers, need to know that
the code to be pulled will be satisfactory in this regard.

It should be considered normal to send more than one round of a patchset, based
on review comments that hopefully do not need to point out issues in anything
of the above. Rather focus on design or potential uses and other impact.

Kernel patches
^^^^^^^^^^^^^^

Workflow is described at https://github.com/btrfs/btrfs-workflow .

Suggested branch names for patchsets for current development cycle:

-  **base** -- the last commit of the last pull (could be in branch
   named something like **misc-next**), or some older
   head of pull request

Patches for next development cycle:

-  **base** -- the last release candidate tag in Linus' tree, be sure
   not to be ahead of the integration branches that will become the pull
   requests for the next development cycle.
-  **for-next** -- patches should be in a good state, i.e. don't
   complicate testing too much, workarounds or known problems should be
   documented (e.g. in the patchset cover letter)
-  other names, for example a patchset for a given feature as a topic
   branch: **feature-live-repair**

Experimental, unsafe or unreviewed patchsets are good candidates for topic
branches as they could be added or removed from the for-next branches easily
(compared to manually removing the patches from a long series).

Btrfs-progs patches
^^^^^^^^^^^^^^^^^^^

The first paragraph from previous section applies here as well.

Unlike the kernel, there are no release candidates during development.  If a
patchset is independent, the *master* branch is a suitable point.  In case
there are other patches in *devel*, a non-rebased development branch needs to
be created. As this is not needed most of the time, this will happen only
on-demand.

Copyright notices in files, SPDX
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**License information in files is using the SPDX standard.**

Quoting https://spdx.dev/about/:

   SPDX is an open standard for communicating software bill of material
   information, including provenance, license, security, and other
   related information. SPDX reduces redundant work by providing common
   formats for organizations and communities to share important data,
   thereby streamlining and improving compliance, security, and
   dependability. The SPDX specification is recognized as the
   international open standard for security, license compliance, and
   other software supply chain artifacts as ISO/IEC 5962:2021.

The initiative started in 2017 https://lwn.net/Articles/739183/ aims to
unify licensing information in all files using **SPDX** tags, this is driven by
the Linux Foundation. Therefore it's not necessary to repeat the license header
(GPL) in each file, the licensing rules are documented in
https://docs.kernel.org/process/license-rules.html (also available in linux git
tree in Documentation/process/license-rules.rst which is the source file of the
linked document).

Complete information with history about contributors of changes in a particular
file is recorded in **git** using **Signed-off-by** tags that are documented
and widely understood (Developer's certificate of origin). For more information
please see
https://docs.kernel.org/process/submitting-patches.html#sign-your-work-the-developer-s-certificate-of-origin
.

**Copyright notices in files.** This delves into the legal territory and
there's no easy answer about the recommended practice. This paragraph could
help you to answer some questions regarding that but is not by any means
complete and refers to known and documented practices:

-  All code merged into the mainline kernel retains its original
   ownership (https://docs.kernel.org/process/1.Intro.html#licensing)
-  Removing the copyright from existing files is not trivial and would
   require asking the original authors or current copyright holders.
-  In btrfs code, adding copyright notices is not mandatory
-  In btrfs code, there are some copyright notices present from years
   before 2017
-  In btrfs code, the copyright notices are inconsistent and incomplete
   (please refer to git history to look up the cod change authors)
-  If not sure, please consult your lawyers

**Btrfs developer community perspective, not legally binding.**

The copyright notices are not required and are discouraged for reasons that are
practical rather than legal. The files do not track all individual contributors
nor companies (this can be found in git), so the inaccurate and incomplete
information gives a very skewed if not completely wrong idea about the
copyright holders of changes in a given file. The code is usually heavily
changed over time in smaller portions, slowly morphing into something that does
not resemble the original code anymore though it shares a lot of the core ideas
and implemented logic.  A copyright notice by a company that does not exist
anymore from 10 years ago is a clear example of uselesness for the developers.

When code is moved verbatim from a file to another file, in the new file it
appears to be contributed by a single author while it is in most cases code
resulting from many previous contributions from other people.  This is most
obvious when splitting code from big files to new ones, because this is
considered a good development practice, but somehow goes against the meaning of
the copyright notices, unless a complete list of original code authors and
copyright holders is also copied.

The current copyright notices will not be removed but at least new
contributions won't continue adding new ones. The change history is recorded in
git using Signed-off-by tags that are documented
(https://docs.kernel.org/process/submitting-patches.html#sign-your-work-the-developer-s-certificate-of-origin)
and widely understood. Unless there's a blessed practice regarding the
copyright notices that significantly improves the situation, the current way is
considered sufficient for practical development purposes.

Given all the above, we don't want the copyright notices in individual files
that are new, renamed or split. This applies to all new changes in all btrfs
related code changes. Obviously lawyers may disagree and/or may require
additional refinements to the process, which is fine but beyond the scope of
this section.

You may also read a perspective from Linux Foundation that shares a similar
view:
https://www.linuxfoundation.org/blog/copyright-notices-in-open-source-software-projects/

.. _devfaq-development-schedule:

Development schedule
--------------------

A short overview of the development phases of linux kernel and what this means
for developers regarding sending patches etc.

Major release
~~~~~~~~~~~~~

*Overall:* a major release is done by Linus, the version bumps in the 2nd
position of the version, e.g. it's *4.6*. This usually means distributions
start to adopt the sources, the stable kernels are going to be released.

*Developers:* expect bug reports based on this version, this usually does not
have other significance regarding development of new features or bugfixes

Merge window
~~~~~~~~~~~~

*Overall:* the time when pull requests from 1st level maintainers get sent to
Linus, the merge window starts after the major release and usually takes two
weeks

*Developers:* get ready with any bugfixes that were not part of the patches in
the pull requests but are still relevant for the upcoming kernel

There are usually one or two pull requests sent by the maintainer so it's OK to
send the bugfixes to the mailinglist even during the merge window period. If
the "deadline" is not met, the patches get merged in the next *rc*.

Sending big patchsets during this period is not discouraged, but feedback may
be delayed.

The amount of changes that go to *master* branch from the rest of the kernel is
high, things can break due to reasons unrelated to btrfs changes. Testing is
welcome, but the bugs could turn out not to be relevant to us.

The rc1
~~~~~~~

*Overall:* most of the kernel changes are now merged, no new features are
allowed to be added, the following period until the major release is expected
to fix only regressions

*Developers:* it's a good time to test extensively, changes in VFS, MM,
scheduler, debugging features and other subsystems could introduce bugs or
misbehaviour

From now on until the late release candidates, it's a good time to post big
patchsets that are supposed to land in the next kernel. There's time to let
others to do review, discuss design goals, do patchset revisions based on
feedback.

Depending on the proposed changes, the patchset could be queued for the next
release within that time. If the patchset is intrusive, it could stay in the
*for-next* branches for some time.

The late rcX (rc5 and up)
~~~~~~~~~~~~~~~~~~~~~~~~~

*Overall:* based on past experience, there are at least 5 release candidates,
done on a weekly basis, so you can estimate the amount of time before the full
release or merge window. The 5 seems like am minimum, usually there are 2 or 3
more release candidates.

*Developers:* new code for the upcoming kernel is supposed to be reviewed and
tested, can be found in the *for-next* branch

Sending intrusive changes at this point is not guaranteed to be reviewed or
tested in time so it gets queued for the next kernel. This highly depends on the
nature of the changes. Patch count should not be an issue if the patches are
revieweable or do not do intrusive changes.

Last rcX before major release (rc7 or rc8)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The releases typically take 3 months, which means that rc7 or rc8 are the
last ones, followed by a release and the merge window opens. Before that the
development is effectively frozen or continues in parallel. Up to date list of
release dates is at https://en.wikipedia.org/wiki/Linux_kernel_version_history .

Major release
~~~~~~~~~~~~~

``goto 1;``

Development phase, linux-next, for-next
---------------------------------------

Patches and patchsets that are supposed to be merged in the next merge cycle
are usually collected in the linux-next git tree. This gives an overview about
potential conflicts and provides a central point for testing various patches.
The btrfs patches for linux-next tree are hosted at
https://git.kernel.org/pub/scm/linux/kernel/git/kdave/linux.git in branch
*for-next*. The update period is irregular, usually a few times per week.

Patches are added to for-next when they get a basic review and do not seriously
decrease stability. Some level of breakage is allowed and inevitable so there's
a possibility to get a tree for early testing.  Also there are external
services that provide compilation coverage for various arches and
configurations.

The for-next branch is rebased and rebuilt from scratch and cannot be used as
base for patch development. Independent patches should use last -rc tag.

-  https://git.kernel.org/pub/scm/linux/kernel/git/next/linux-next.git
   -- daily snapshots
-  https://git.kernel.org/pub/scm/linux/kernel/git/kdave/linux.git --
   for-next

Misc information and hints
--------------------------

This section collects random pieces of advice from mailinglist that are given
to newcomers.

How to get started - development
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

-  Build and install the latest kernel from Linus's git repository.
-  Create one or several btrfs filesystems with different configurations
   and learn how they work in userspace -- what are the features, what
   are the problems you see? Actually use at least one of the
   filesystems you created for real data in daily use (with backups)
-  Build the userspace tools from git
-  Project ideas used to be tracked on the wiki
   (https://archive.kernel.org/oldwiki/btrfs.wiki.kernel.org/index.php/Project_ideas.html)
   but this contains outdated information and will be moved elsewhere eventually.
   If you pick the right one(s), you'll have to
   learn about some of the internal structures of the filesystem anyway. Compile
   and test your patch. If you're adding a new feature, write an
   automated fstests case for it as well.
-  Get that patch accepted. This will probably involve a sequence of
   revisions to it, multiple versions over a period of several weeks or
   more, with a review process. You should also send your test to
   fstests and get that accepted.
-  Do the above again, until you get used to the processes involved, and
   have demonstrated that you can work well with the other people in the
   subsystem, and are generally producing useful and sane code. It's all
   about trust -- can you be trusted to mostly do the right thing?
-  Developer documentation is listed in a section on the main documentation page.
-  Output of *btrfs inspect-internal dump-tree* can be helpful to understand
   the internal structure of the filesystem.

How not to start
~~~~~~~~~~~~~~~~

It might be tempting to look for coding style violations and send patches to
fix them. This happens from time to time and the community does not welcome
that. The following text reflects our stance:

If you want to contribute and do something useful for others and yourself, just
don't keep sending these patches to fix whitespace/style issues reported by
checkpatch.pl. Think about it:

#. You don't learn anything by doing them. You don't learn nothing about btrfs
   internals, filesystems in general, kernel programming in general, general
   programming in C, etc. It ends up being only a waste of time for you;
#. You're not offering any benefit to users - not fixing a bug, not adding a
   new feature, not doing any performance/efficiency improvement, not making
   the code more reliable, etc;
#. You're not offering a benefit for other developers either, like doing a
   cleanup that simplifies a complex algorithm for example.

If you care so much about the whitespace/style issues, just fix them while
doing a useful change as mentioned above that happens to touch the same code.
It takes time to read and understand code, it can be a big investment of time,
but it ends up being worth it. There's plenty of bug reports and performance
issues in the mailing list or bugzilla, so there's no shortage of things to do.

Same advice applies to any other kernel subsystem or open source project in
general. Also before jumping into such a storm of useless patches, observe
first what a community does for at least a month, and learn from other
contributors - what they do, how they do it, the flow of development and
releases, etc. Don't rush into a sending patch just for the sake of sending it
and having your name in the git history.

References
----------

-  `Kernel maintainership: an oral
   tradition <https://bootlin.com/pub/conferences/2015/elce/clement-kernel-maintainership-oral-tradition/clement-kernel-maintainership-oral-tradition.pdf>`__
   (pdf) a nice presentation from ELCE 2015 what does it mean to be a
   maintainer and what the developers can expect.
-  https://www.kernel.org/doc/html/latest/process/submitting-patches.html
   (must read)
-  https://www.kernel.org/doc/html/latest/process/coding-style.html
   (must read)
-  Pro Git by Scott Chacon http://progit.org/book/
-  Git project main page http://git-scm.com
