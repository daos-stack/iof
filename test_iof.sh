#!/bin/sh

set -e
set -x

. ./setup_local.sh

echo Trying to run Mercury ping test.

if [ -n "$IOF_USE_VALGRIND" ]; then
  CMD_PREFIX="valgrind --xml=yes --xml-file=valgrind.%q{PMIX_RANK}.xml --leak-check=yes"
fi

BUILD_DIR=./build/iof

orterun --tag-output -np 1  $CMD_PREFIX $BUILD_DIR/proto/ping/test_rpc_server : \
	-np 1 $CMD_PREFIX $BUILD_DIR/proto/ping/test_rpc_client

echo Trying to run process set tests.

orterun --tag-output -np 4 $CMD_PREFIX $BUILD_DIR/proto/process_set/test_ps --name service_set --is_service 1 : \
	-np 4 $CMD_PREFIX $BUILD_DIR/proto/process_set/test_ps --name client_set --is_service 0 --attach_to service_set

orterun --tag-output -np 1 $CMD_PREFIX $BUILD_DIR/proto/process_set/test_ps --name a --is_service 1 : \
	-np 1 $CMD_PREFIX $BUILD_DIR/proto/process_set/test_ps --name b --attach-to c : \
	-np 1 $CMD_PREFIX $BUILD_DIR/proto/process_set/test_ps --name a --is_service 1 : \
	-np 2 $CMD_PREFIX $BUILD_DIR/proto/process_set/test_ps --name c --is_service --attach-to a

orterun --tag-output -np 4 $CMD_PREFIX $BUILD_DIR/proto/process_set/test_ps --name test_srv_set --is_service 1

set +e

./$BUILD_DIR/proto/iof_prototype/server_main &

SERVER_PID=$!

[ -d child_fs ] || mkdir child_fs

./$BUILD_DIR/proto/iof_prototype/client_main child_fs

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
cd ..



if [ "$os" = "Darwin" ];then
    sudo umount child_fs
else
    fusermount -u child_fs
fi

/bin/kill -9 $SERVER_PID
wait

