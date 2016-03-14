#!/bin/sh

set -e
set -x

if [ -z "$WORKSPACE" ]; then
  WORKSPACE=`pwd`
fi

os=`uname`
if [ "$os" = "Darwin" ]; then
  export DYLD_LIBRARY_PATH=$DYLD_LIBRARY_PATH:${WORKSPACE}/install/lib
else
  export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:${WORKSPACE}/install/lib
fi
export PATH=${WORKSPACE}/install/bin:$PATH

echo Trying to run Mercury ping test.

if [ -n "$IOF_USE_VALGRIND" ]; then
  CMD_PREFIX="valgrind --xml=yes --xml-file=valgrind.%q{PMIX_RANK}.xml --leak-check=yes"
fi

BUILD_DIR=./build/iof/

orterun --tag-output -np 1  $CMD_PREFIX $BUILD_DIR/proto/ping/test_rpc_server : \
-np 1 $CMD_PREFIX $BUILD_DIR/proto/ping/test_rpc_client
