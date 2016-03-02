#!/bin/sh

set -e
set -x

prefix=`readlink -f /scratch/coral/artifacts/scons-local-update-scratch/latest`
rm -f *.conf
scons PREBUILT_PREFIX=$prefix -c
scons
scons install
