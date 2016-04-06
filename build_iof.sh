#!/bin/sh

set -e
set -x

#Links are resolved by prereq_tools and target is saved
MERCURY=${CORAL_ARTIFACTS}/mercury-update-scratch/latest
OMPI=${CORAL_ARTIFACTS}/ompi-update-scratch/latest

rm -f *.conf
scons PREBUILT_PREFIX=${MERCURY}:${OMPI} -c
scons
scons install
