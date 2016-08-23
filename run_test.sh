#!/bin/sh
set -e

# Run the tests from the Jenkins build job so that if there is a test failure
# the "latest" symlink is not created.
if [ -n "$IOF_USE_VALGRIND" ]; then
  scons utest --utest-mode=memcheck
  python3.4 test/iof_test_fs.py memcheck
  mv ${WORKSPACE}/testLogs/test_fs_*/valgrind.*.xml .
  python3.4 test/iof_test_ionss.py memcheck
  mv ${WORKSPACE}/testLogs/nss_*/valgrind.*.xml .
else
  scons utest
  python3.4 test/iof_test_fs.py
  python3.4 test/iof_test_ionss.py
fi
