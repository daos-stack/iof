#!/bin/sh

set -e
set -x

export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:${WORKSPACE}/iof/install/lib
export PATH=${WORKSPACE}/iof/install/bin:$PATH

echo Trying to run pmix tests.
orterun -np 2 ../pmix/examples/client

cd ping
make
echo Trying to run Mercury tests.
orterun -np 1 ./test_rpc_server : -np 1 ./test_rpc_client
