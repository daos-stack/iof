#!/bin/sh

set -e
set -x

prefix=`readlink -f /scratch/hudson/mercury/mercury-update-scratch/latest`
scons PREBUILT_PREFIX=$prefix
scons install PREBUILT_PREFIX=$prefix
