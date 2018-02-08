#!/bin/bash
# export the testsuite files to a separate tar

if ! [ -f testsuite-files ]; then
	echo "ERROR: cannot find testsuite-files"
	exit 1
fi

set -e

TESTSUITE_TAR="btrfs-progs-tests.tar.gz"
rm -f "$TESTSUITE_TAR"

TIMESTAMP=`date -u "+%Y-%m-%d %T %Z"`

{
	echo "VERSION=`cat ../VERSION`"
	echo "GIT_VERSION=`git describe`"
	echo "TIMESTAMP='$TIMESTAMP'"
} > testsuite-id

# Due to potentially unwanted files in the testsuite (restored images or other
# temporary files) we can't simply copy everything so the tar
#
# The testsuite-files specifier:
# F file
#   - directly copy the file from the given path, may be a git tracked file or
#     a built binary
# G path
#   - a path relative to the top of git, recursively traversed; path
#     postprocessing is needed so the tar gets it relative to tests/
while read t f; do
	case "$t" in
		F) echo "$f";;
		G)
			here=`pwd`
			cd ..
			git ls-tree -r --name-only --full-name HEAD "$f" |
				sed -e 's#^tests/##' |
				sed -e 's#^Documentation#../Documentation#'
			cd "$here"
			;;
	esac
done < testsuite-files > testsuite-files-all

echo "create tar: $TESTSUITE_TAR"
tar cz --sparse -f "$TESTSUITE_TAR" -T testsuite-files-all
if [ $? -eq 0 ]; then
	echo "tar created successfully"
	cat testsuite-id
	rm -f testsuite-files-all
	rm -f testsuite-id
else
	exit $?
fi
