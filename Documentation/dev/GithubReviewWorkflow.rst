Pull request review workflow
============================

Code changes can be sent as pull requests (PR) in the Github web UI. Some
integration tests are run and then the PR waits for a review, approval and
merge.

Open the pull requests against branch **devel** (against *master* branch it's
also possible but this may miss other development changes).

The merge strategy is to *rebase and merge*. This means that the changes are
applied on top of the current development branch which is **devel**, although
they could have been originally based on a different commit in your local
repository.

Reviewer
--------

In the pull request tab *File changes* go through the diff. If you want to leave
a comment for a particular line, click the plus in a blue box (``[+]``) . Write
text about the problem, what needs to be fixed or change. Clarification
requests are also ok (not necessarily leading to a change). This will add a
single review comment/item.

Adding such comments will add them to the PR code but it's not visible to the
submitter until you click a *Review changes* at the top of the file diffs. Once
all comments are written for this round, they need to be submitted by writing
the summary review comment.

Approved
^^^^^^^^

If everything is OK and no review comments need to be resolved, write a comment
and approve the whole PR. This will be noted in the *Conversation* as a comment
and visible in the PR list with *Approved* text.

Changes requested
^^^^^^^^^^^^^^^^^

Assuming there are review comments, this will mark the whole PR, a comment is
added to the *Conversation* (publishing the comments).

Submitter
---------

If you have email notifications on, you'll get notified about new review
comments or about PR status changes (like that it got merged).

Please check the review comments and either explain why things need to be done
in such and such way or fix it in your code and mark the comment as *Resolved*.
Any changes in the code need to be done locally and then pushed to your
repository, the updates will be logged in the overview.

Review comments on lines that did not change will be probably visible in the
new branch updates, resolved on changed lines will disappear (but will be still
listed in the previous review summary).

Checklist
---------

* set the desired target *Milestone* before closing
* check the result in git branch after merge for potential clashing changes
  that were not discovered in the meantime
* you can still fix up code after merge and push, but do this carefully so it
  does not overwrite other changes
* review comments can be *Unresolved*, use that cautiously so it does not cause
  confusion

References
----------

* https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/reviewing-changes-in-pull-requests/about-pull-request-reviews
* https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/reviewing-changes-in-pull-requests/reviewing-proposed-changes-in-a-pull-request
