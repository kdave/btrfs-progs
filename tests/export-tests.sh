#!/bin/bash
# export the testsuite files to a separate tar

TESTSUITES_LIST_FILE=$PWD/testsuites-list
if ! [ -f $TESTSUITES_LIST_FILE ];then
	echo "testsuites list file is not exsit."
	exit 1
fi

TESTSUITES_LIST=$(cat $TESTSUITES_LIST_FILE)
if [ -z "$TESTSUITES_LIST" ]; then
	echo "no file be list in testsuites-list"
	exit 1
fi

DEST="btrfs-progs-tests.tar.gz"
if [ -f $DEST ];then
	echo "remove exsit package: " $DEST
	rm $DEST
fi

TEST_ID=$PWD/testsuites-id
if [ -f $TEST_ID ];then
	rm $TEST_ID
fi
VERSION=`./version.sh`
TIMESTAMP=`date -u "+%Y-%m-%d %T %Z"`

echo "git version: " $VERSION > $TEST_ID
echo "this tar is created in: " $TIMESTAMP >> $TEST_ID

echo "begin create tar:  " $DEST
tar --exclude-vcs-ignores -zScf $DEST -C ../ $TESTSUITES_LIST
if [ $? -eq 0 ]; then
	echo "create tar successfully."
fi
rm $TEST_ID
