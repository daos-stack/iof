#!/bin/sh

set -e
set -x

. ./setup_local.sh

echo Trying to run Mercury ping test.

if [ -n "$IOF_USE_VALGRIND" ]; then
  CMD_PREFIX="valgrind --xml=yes --xml-file=valgrind.%q{PMIX_RANK}.xml --leak-check=yes"
fi

BUILD_DIR=./build/iof/

orterun --tag-output -np 1  $CMD_PREFIX $BUILD_DIR/proto/ping/test_rpc_server : \
-np 1 $CMD_PREFIX $BUILD_DIR/proto/ping/test_rpc_client
