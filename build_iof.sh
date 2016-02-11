#!/bin/sh

set -e
set -x

prefix=`readlink -f /scratch/coral/artifacts/mercury-update-scratch/latest`
rm -f *.conf
scons PREBUILT_PREFIX=$prefix
scons install
