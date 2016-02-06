#!/bin/sh

# Clean up any failed previous tests,  Currently just umounts the filesystem.
#
# Do not use set -e as this should continue on errors.

os=`uname`
if [ "$os" = "Darwin" ];then
    sudo umount child_fs
else
    fusermount -u child_fs
fi
