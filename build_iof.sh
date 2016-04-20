#!/bin/sh

set -e
set -x

option=
IOF_INSTALL=
#Links are resolved by prereq_tools and target is saved
MERCURY=${CORAL_ARTIFACTS}/mercury-update-scratch/latest
OMPI=${CORAL_ARTIFACTS}/ompi-update-scratch/latest

if [ -n "$WORKSPACE" ]; then
export IOF_INSTALL="${CORAL_ARTIFACTS}/${JOB_NAME}/${BUILD_NUMBER}"
PREBUILT_AREA=${MERCURY}:${OMPI}
option="PREBUILT_PREFIX=${PREBUILT_AREA} PREFIX=${IOF_INSTALL}/iof"
fi
scons $option
scons install

if [ -n "${IOF_INSTALL}" ]; then
ln -sfn ${IOF_INSTALL} ${CORAL_ARTIFACTS}/${JOB_NAME}/latest
fi
