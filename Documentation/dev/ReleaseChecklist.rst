Release checklist
=================

Last code touches:

*  make the code ready, collect patches queued for the release
*  look to mailinglist for any relevant last-minute fixes
*  skim patches for typos, inconsistent subjects

Pre-checks:

*  update package in OBS, (multi arch build checks)
*  run all functional tests locally with

   *  defaults
   *  D=asan
   *  D=ubsan
*  run all build tests (``tests/build-tests.sh``)
*  run with fstests
*  check Github actions for status (https://github.com/kdave/btrfs-progs/actions)

   *  branch *devel*
   *  branch *release-test* -- extensive pre-release build checks
   *  branch *coverage-test* -- code coverage, for information purposes only

Pre-release:

*  write CHANGES entry (will be visible on RTD right away)

Python btrfsutil (pypi.org):

*  rebuild whole project (regenerate constants.c and version.py)
*  ``cd libbtrfsutil/python``
*  ``python3 -m build`` -- build dist files
*  ``twine check dist/*.tar.gz`` -- look for warnings
*  ``twine upload dist/*.tar.gz`` -- make sure there's only the latest version,
   *twine* must need access token to pypi.org

Release:

*  tag release, sign
*  build check of unpacked tar
*  generate documentation
*  make tar
*  upload tar to kernel.org
*  refresh git branches, push tags

Post-release:

*  write and send announcement mail to the mailinglist
*  update title on IRC
*  github updates

   *  create a new release from the latest tag
   *  copy text from CHANGES as contents, formatting is the same
   *  wait for static binaries github action to finish
   *  run ``ci/actions/update-artifacts`` to copy the built static binaries to the
      release (requires github command line tool ``gh``)
