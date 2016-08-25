#!/bin/sh
set -e

# Run the tests from the Jenkins build job so that if there is a test failure
# the "latest" symlink is not created.
if [ -z "$IOF_TEST_MODE"  ]; then
  IOF_TEST_MODE="native"
fi

if [[ "$IOF_TEST_MODE" =~ (native|all) ]]; then
  scons utest
  python3 test/iof_test_fs.py
  python3 test/iof_test_ionss.py
fi

if [[ "$IOF_TEST_MODE" =~ (memcheck|all) ]]; then
  scons utest --utest-mode=memcheck
  python3 test/iof_test_fs.py memcheck
  python3 test/iof_test_ionss.py memcheck
fi
