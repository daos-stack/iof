#!/bin/sh

set -e
set -x

. ./setup_local.sh

echo Trying to run pmix tests.
orterun -np 2 ./_build.external/pmix/examples/client
