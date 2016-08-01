#!/bin/sh
set -e

# Run the tests from the install TESTING directory
if [ -z "$IOF_TEST_MODE"  ]; then
  IOF_TEST_MODE="native"
fi

if [ -n "$COMP_PREFIX"  ]; then
  TESTDIR=${COMP_PREFIX}/TESTING
else
  TESTDIR="install/Linux/TESTING"
fi
if [[ "$IOF_TEST_MODE" =~ (native|all) ]]; then
  scons utest
  cd ${TESTDIR}
  python3.4 test_runner scripts/iof_test_fs.yml scripts/iof_test_ionss.yml
  cd -
fi

if [[ "$IOF_TEST_MODE" =~ (memcheck|all) ]]; then
  scons utest --utest-mode=memcheck
  export TR_USE_VALGRIND="memcheck"
  cd ${TESTDIR}
  python3.4 test_runner scripts/iof_test_fs.yml scripts/iof_test_ionss.yml
  cd -
fi
