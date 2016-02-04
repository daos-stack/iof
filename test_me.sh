#!/bin/sh

set -e
set -x

if [ -z "$WORKSPACE" ]; then
  WORKSPACE=`readlink -f ..`
fi

os=`uname`
if [ "$os" = "Darwin" ]; then
export DYLD_LIBRARY_PATH=$DYLD_LIBRARY_PATH:${WORKSPACE}/iof/install/lib
else
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:${WORKSPACE}/iof/install/lib
fi
export PATH=${WORKSPACE}/iof/install/bin:$PATH

echo Trying to run pmix tests.
orterun -np 2 ./build/pmix/examples/client

echo Trying to run Mercury tests.
orterun -np 1 ./build/ping/test_rpc_server : -np 1 ./build/ping/test_rpc_client
