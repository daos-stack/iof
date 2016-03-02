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

export OPAL_PREFIX=${WORKSPACE}/iof/install

echo Trying to run pmix tests.
orterun -np 2 ./_build.external/pmix/examples/client
