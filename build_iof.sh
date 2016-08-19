#!/bin/sh

set -e
set -x

# Jenkins build script for IOF, this should not be executed directly.
#
# In this file .build_vars has a arch suffix for local uses however when
# installed to ${CORAL_ARTIFACTS} it does not, neither does the .build_vars
# used from the MCL install.

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
      #These can be used to override the "last good version"
      #when there are breaking changes.   Jenkins will use
      #$GOOD_* only if it is newer than the last version
      #used by master
      GOOD_MCL=${CORAL_ARTIFACTS}/mcl-update-scratch/194
      GOOD_OMPI=${CORAL_ARTIFACTS}/ompi-update-scratch/89
      GOOD_MERCURY=${CORAL_ARTIFACTS}/mercury-update-scratch/125
      source ${vars}
      for lib in MERCURY OMPI MCL; do
        blessed_varname=SL_${lib}_PREFIX
        good_varname=GOOD_${lib}
        blessed_num=$(basename $(dirname ${!blessed_varname}))
        good_num=`basename ${!good_varname}`
        if [ $good_num -gt $blessed_num ]; then
          declare $lib=${!good_varname}
        else
          declare $lib=$(dirname ${!blessed_varname})
        fi
      done
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

scons utest
scons utest --utest-mode=memcheck

# Run the tests from the Jenkins build job so that if there is a test failure
# the "latest" symlink is not created.
if [ -n "$IOF_USE_VALGRIND" ]; then
  python3.4 test/iof_test_fs.py memcheck
  mv ${WORKSPACE}/testLogs/test_fs_*/valgrind.*.xml .
  python3.4 test/iof_test_ionss.py memcheck
  mv ${WORKSPACE}/testLogs/nss_*/valgrind.*.xml .
else
  python3.4 test/iof_test_fs.py
  python3.4 test/iof_test_ionss.py
fi

if [ -n "${IOF_INSTALL}" ]; then
  cp .build_vars-`uname -s`.sh ${IOF_INSTALL}/.build_vars.sh
  cp .build_vars-`uname -s`.json ${IOF_INSTALL}/.build_vars.json
  ln -sfn ${IOF_INSTALL} ${CORAL_ARTIFACTS}/${JOB_NAME}/latest
fi

