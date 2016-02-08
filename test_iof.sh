#!/bin/sh

set -e
set -x

if [ -z "$WORKSPACE" ]; then
  WORKSPACE=`readlink -f .`
fi

os=`uname`
if [ "$os" = "Darwin" ]; then
export DYLD_LIBRARY_PATH=$DYLD_LIBRARY_PATH:${WORKSPACE}/install/lib
else
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:${WORKSPACE}/install/lib
fi
export PATH=${WORKSPACE}/install/bin:$PATH

echo Trying to run Mercury tests.
orterun -np 1  ./build/ping/test_rpc_server : -np 1 ./build/ping/test_rpc_client
