#!/bin/sh

set -e
set -x

[ -d build ] && rm -rf build
[ -d install ] && rm -rf install
mkdir -p build
mkdir -p install

./build_mercury.sh
./build_pmix_ompi.sh
