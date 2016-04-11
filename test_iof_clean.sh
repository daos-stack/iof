#!/bin/sh

# Clean up any failed previous tests,  Currently just umounts the filesystem.
#
# Do not use set -e as this should continue on errors.

os=`uname`
MOUNT_DIR=child_fs
if find $MOUNT_DIR/ -maxdepth 0 -empty | read v;then
	echo "Clean"
else
for entry in "$MOUNT_DIR"/*
do
		echo "Found $entry"
		if  mount | grep -q "$entry" ;
		then
			echo "Unmounting"
			if [ "$os" = "Darwin" ];then
				sudo umount $entry
			else
				fusermount -u $entry
			fi
		fi
		echo "Removing $entry"
		rm -r $entry
done
fi

