#!/bin/sh

set -e
set -x

. ./setup_local.sh

echo Trying to run Mercury ping test.

if [ -n "$IOF_USE_VALGRIND" ]; then
CMD_PREFIX="valgrind --xml=yes --xml-file=valgrind.%q{PMIX_ID}.xml "
CMD_PREFIX="$CMD_PREFIX --leak-check=yes --suppressions=${SL_MCL_PREFIX}/etc/memcheck-mcl.supp "
CMD_PREFIX="$CMD_PREFIX --show-reachable=yes"
fi

# Just launch the program for now and it'll exit cleanly.  More will be needed
# here when the CNSS and IONSS processes actually do anything.
orterun --tag-output -np 4 $CMD_PREFIX cnss : -np 3 $CMD_PREFIX ionss

# Disable automatic exit whilst running FUSE so that we can attempt to
# shutdown correctly.  Instead for FUSE verify that we can create
# and then read back the target of a sym link.
set +e
#Sometimes if things go wrong the directory inside child_fs may not get deleted
# and will stop the client process because it creates new directories for
#mounting.
rm -r child_fs
mkdir child_fs

N=2

orterun --tag-output -np 1 $CMD_PREFIX client_main -mnt child_fs \
		: -np $N $CMD_PREFIX server_main &

ORTE_PID=$!
c=0
while [ $c -lt $N ]
	do
	MOUNT_DIR=child_fs/Rank$c
	[ -d $MOUNT_DIR/started ] || sleep 1
	[ -d $MOUNT_DIR/started ] || sleep 2
	[ -d $MOUNT_DIR/started ] || sleep 4
	[ -d $MOUNT_DIR/started ] || sleep 8
	[ -d $MOUNT_DIR/started ] || sleep 16
	if [ -d $MOUNT_DIR/started ];
	then
		ls $MOUNT_DIR
		cd $MOUNT_DIR
		mkdir d e
		rm -r e
		ls
		ln -s d d_sym
		ls
		rm d_sym
		ls
		rm -r d
		ln -s target origin
		LINK=`readlink origin`
		cd -
	else
		LINK="none"
		echo "Filesystem not mounted correctly"
		break
	fi
	((c++))
done

#Test for fuse loop exit using extended attributes
c=0
while [ $c -lt $N ]
do
	MOUNT_DIR=child_fs/Rank$c
	if [ "$os" = "Darwin" ];then
		xattr -w user.exit 1 $MOUNT_DIR
	else
		setfattr -n user.exit -v 1 $MOUNT_DIR
	fi
		((c++))
done


/bin/kill -TERM $ORTE_PID
sleep 2

c=0
while [ $c -lt $N ]
do
	MOUNT_DIR=child_fs/Rank$c
	if mount | grep -q "$MOUNT_DIR"
	then
		if [ "$os" = "Darwin" ];then
			umount $MOUNT_DIR
		else
			fusermount -u $MOUNT_DIR
		fi
	fi
	if [ -h $MOUNT_DIR/origin ]
	then
		echo "Unmount unsuccessful"
		exit 1
	fi
	rm -r $MOUNT_DIR
	((c++))
done

/bin/kill -TERM $ORTE_PID
sleep 1
/bin/kill -TERM $ORTE_PID
wait

#Verify if filesystem failed
if [ "$LINK" != "target" ]
then
	exit 1
fi
