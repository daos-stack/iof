#!/bin/sh

set -e
set -x

option=
IOF_INSTALL=

if [ -n "$WORKSPACE" ]; then
#Links are resolved by prereq_tools and target is saved
  MERCURY=${CORAL_ARTIFACTS}/mercury-update-scratch/latest
  OMPI=${CORAL_ARTIFACTS}/ompi-update-scratch/latest
  MCL=${CORAL_ARTIFACTS}/mcl-update-scratch/latest
  export IOF_INSTALL="${CORAL_ARTIFACTS}/${JOB_NAME}/${BUILD_NUMBER}"
  if [ "${JOB_NAME}" != "iof-update-scratch" ]; then
    latest=$(readlink -f ${CORAL_ARTIFACTS}/iof-update-scratch/latest)
    vars=${latest}/.build_vars.sh
    if [ -f ${vars} ]; then
      #Use the last good version
      source ${vars}
      MERCURY=${SL_MERCURY_PREFIX}/..
      OMPI=${SL_OMPI_PREFIX}/..
      MCL=${SL_MCL_PREFIX}/..
    fi
  else
    latest=$(readlink -f ${MCL})
    vars=${latest}/.build_vars.sh
    if [ -f ${vars} ]; then
      #Use the last good version
      source ${vars}
      MERCURY=${SL_MERCURY_PREFIX}/..
      OMPI=${SL_OMPI_PREFIX}/..
      MCL=${latest}
    fi
  fi

  PREBUILT_AREA=${MERCURY}:${OMPI}:${MCL}
  option="PREBUILT_PREFIX=${PREBUILT_AREA} PREFIX=${IOF_INSTALL}/iof"

fi
scons $option
scons install

# Run the tests from the Jenkins build job so that if there is a test failure
# the "latest" symlink is not created.
./test_iof.sh

if [ -n "${IOF_INSTALL}" ]; then
  cp .build_vars.sh ${IOF_INSTALL}/.
  cp .build_vars.py ${IOF_INSTALL}/.
  ln -sfn ${IOF_INSTALL} ${CORAL_ARTIFACTS}/${JOB_NAME}/latest
fi

