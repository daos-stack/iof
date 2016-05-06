#!/bin/sh

set -e
set -x

option=
IOF_INSTALL=
#Links are resolved by prereq_tools and target is saved
MERCURY=${CORAL_ARTIFACTS}/mercury-update-scratch/latest
OMPI=${CORAL_ARTIFACTS}/ompi-update-scratch/latest
MCL=${CORAL_ARTIFACTS}/mcl-update-scratch/latest

if [ -n "$WORKSPACE" ]; then
export IOF_INSTALL="${CORAL_ARTIFACTS}/${JOB_NAME}/${BUILD_NUMBER}"
PREBUILT_AREA=${MERCURY}:${OMPI}:${MCL}
option="PREBUILT_PREFIX=${PREBUILT_AREA} PREFIX=${IOF_INSTALL}/iof"
fi
scons --no-prereq-links $option
scons --no-prereq-links install

# Run the tests from the Jenkins build job so that if there is a test failure
# the "latest" symlink is not created.
./test_iof.sh

if [ -n "${IOF_INSTALL}" ]; then
ln -sfn ${BUILD_NUMBER} ${CORAL_ARTIFACTS}/${JOB_NAME}/latest
fi
