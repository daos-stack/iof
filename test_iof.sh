#!/bin/sh

set -e
set -x

. ./setup_local.sh

echo Trying to run Mercury ping test.

if [ -n "$IOF_USE_VALGRIND" ]; then
CMD_PREFIX="valgrind --xml=yes --xml-file=valgrind.%q{PMIX_ID}.xml "
CMD_PREFIX="$CMD_PREFIX --leak-check=yes --suppressions=memcheck-pmix.supp "
CMD_PREFIX="$CMD_PREFIX --show-reachable=yes"
fi

orterun --tag-output -np 1  $CMD_PREFIX test_rpc_server : \
	-np 1 $CMD_PREFIX test_rpc_client

echo Trying to run process set tests.

orterun --tag-output -np 4 $CMD_PREFIX test_ps \
        --name service_set --is_service 1 : \
	    -np 4 $CMD_PREFIX test_ps --name client_set --is_service 0 \
        --attach_to service_set

orterun --tag-output -np 1 $CMD_PREFIX test_ps \
    --name a --is_service 1 : \
	-np 1 $CMD_PREFIX test_ps --name b --attach-to c : \
	-np 1 $CMD_PREFIX test_ps --name a --is_service 1 : \
	-np 2 $CMD_PREFIX test_ps --name c --is_service --attach-to a

orterun --tag-output -np 4 $CMD_PREFIX test_ps \
    --name test_srv_set --is_service 1

# Disable automatic exit whilst running FUSE so that we can attempt to
# shutdown correctly.  Instead for FUSE verify that we can create
# and then read back the target of a sym link.
set +e

[ -d child_fs ] || mkdir child_fs

orterun --tag-output -np 1 $CMD_PREFIX client_main -f child_fs \
	: -np 1 $CMD_PREFIX server_main &

ORTE_PID=$!

# The filesystem will be created with an initial directory called "started"
# created so rather than just sleeping for a time poll for this directory to
# appear before attempting to use it.  If the directory does not appear after
# an intiial timeout assume the filesystem is broken.
[ -d child_fs/started ] || sleep 1
[ -d child_fs/started ] || sleep 2
[ -d child_fs/started ] || sleep 4
if [ -d child_fs/started ]
then
  ls
  ls child_fs
  cd child_fs
  mkdir d e
  rm -r e
  ls
  ln -s d d_sym
  ls
  rm -r d
  ls
  rm d_sym
  ls
  ln -s target origin
  LINK=`readlink origin`
  cd ..
else
  LINK="none"
fi

/bin/kill -TERM $ORTE_PID
sleep 2

if [ "$os" = "Darwin" ];then
    umount child_fs
else
    fusermount -u child_fs
fi

if [ -h child_fs/origin ]
then
    exit 1
fi

/bin/kill -TERM $ORTE_PID
sleep 1
/bin/kill -TERM $ORTE_PID
wait

if [ "$LINK" != "target" ]
then
    exit 1
fi
