#!/usr/bin/env python3
"""
iof cnss / ionss test

Usage:

Should be executed from the iof root directory. The results are placed in the
testLogs/nss directory. Any test_ps output is under
testLogs/nss/1(process set)/rank<number>. There you will find anything written
to stdout and stderr. The output from memcheck and callgrind are in the nss
directory. At the end of a test run, the last nss directory is renamed to
nss<date stamp>

python3 test/iof_test_ionss.py

To use valgrind memory checking
python3 test/iof_test_ionss.py memcheck

To use valgrind call (callgrind) profiling
python3 test/iof_test_ionss.py callgrind

"""
#pylint: disable=too-many-locals
#pylint: disable=too-many-branches
#pylint: disable=too-many-statements

import os
import sys
import unittest
import shlex
import subprocess
import json
from datetime import datetime

class Testnss(unittest.TestCase):
    """Simple test"""

    cmd = "orterun --output-filename testLogs/nss"

    def setUp(self):
        print("Testnss: setUp begin")
        print("Testnss: Setting up for simple test.")

        info = {}
        rootpath = os.getcwd()
        print("Testnss: path: %s" % rootpath)
        print("Testnss: input: %s" % sys.argv[0])
        where = os.path.dirname(sys.argv[0])
        print("Testnss: input file: %s" % (where))
        if not where:
            rootpath = ".."
        platform = os.uname()[0]
        opts_file = rootpath + "/.build_vars-%s.py" % platform
        print("Testnss: use file: %s" % opts_file)
        with open(opts_file, "r") as info_file:
            info = json.load(info_file)

        path = os.getenv("PATH")
        if path.find(info['OMPI_PREFIX']) < 0:
            path = info['OMPI_PREFIX'] + "/bin:" + path
        if path.find(info['PREFIX']) < 0:
            path = info['PREFIX'] + "/bin:" + path
        os.environ['PATH'] = path
        os.environ['OMPI_MCA_rmaps_base_oversubscribe'] = "1"
        os.environ['OMPI_MCA_orte_abort_on_non_zero_status'] = "0"
        # Use /tmp for temporary files.  This is required for OS X.
        if platform == "Darwin":
            os.environ['OMPI_MCA_orte_tmpdir_base'] = "/tmp"
            dyld = os.getenv("DYLD_LIBRARY_PATH", default="")
            lib_paths = []
            for var in sorted(info.keys()):
                if not isinstance(info[var], str):
                    continue
                if not "PREFIX" in var:
                    continue
                if info[var] == "/usr":
                    continue
                lib = os.path.join(info[var], "lib")
                lib64 = os.path.join(info[var], "lib64")
                if os.path.exists(lib) and lib not in lib_paths:
                    lib_paths.insert(0, lib)
                if os.path.exists(lib64) and lib64 not in lib_paths:
                    lib_paths.insert(0, lib64)
            os.environ['DYLD_LIBRARY_PATH'] = os.pathsep.join(lib_paths) + dyld

        if len(sys.argv) > 1:
            if sys.argv[1] == "memcheck":
                os.environ['MCL_USE_VALGRIND'] = "memcheck"
                prefix = "valgrind --xml=yes" + \
                    " --xml-file=testLogs/nss/valgrind.%q{PMIX_ID}.xml" + \
                    " --leak-check=yes --show-reachable=yes" + \
                   " --suppressions=" + info['MCL_PREFIX'] + \
                    "/etc/memcheck-mcl.supp"

            if sys.argv[1] == "callgrind":
                os.environ['MCL_USE_VALGRIND'] = "callgrind"
                prefix = "valgrind --tool=callgrind --callgrind-out-file=" + \
                         "testLogs/nss/callgrind.%q{PMIX_ID}.out"
        else:
            prefix = ""
        self.local_server = " -np 4 %s cnss :" % prefix
        self.local_client = " -np 3 %s ionss" % prefix
        if os.path.exists("testLogs/nss"):
            newname = "testLogs/nss_%s" % datetime.now().isoformat()
            os.rename("testLogs/nss", newname)
        os.makedirs("testLogs/nss")
        print("Testnss: setUp end\n")

    def tearDown(self):
        print("Testnss: tearDown begin")
        print("Testnss: Tearing down environment for simple test.")
        if os.getenv("MCL_USE_VALGRIND") == "callgrind":
            srcfile = " %s/src/cnss/*.c" % os.getcwd() + \
                      " %s/src/ionss/*.c" % os.getcwd()
            os.chdir("testLogs/nss")
            dirlist = os.listdir('.')
            for infile in dirlist:
                if os.path.isfile(infile) and infile.find(".out"):
                    outfile = infile.replace("out", "gp.out")
                    cmdarg = "callgrind_annotate " + infile + srcfile
                    print(cmdarg)
                    with open(outfile, 'w') as out:
                        subprocess.call(cmdarg, timeout=30, shell=True,
                                        stdout=out,
                                        stderr=subprocess.STDOUT)

        if os.path.exists("testLogs/nss"):
            newname = "testLogs/nss_%s" % datetime.now().isoformat()
            os.rename("testLogs/nss", newname)
        print("Testnss: tearDown end\n\n")

    def launch_client_server(self):
        """Launch simple test"""
        cmdstr = self.cmd + self.local_server + self.local_client
        cmdarg = shlex.split(cmdstr)
        print("Testnss: input string: %s\n" % cmdstr)
        procrtn = subprocess.call(cmdarg, timeout=30,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        print("Testnss: return string: %d\n" % procrtn)
        return procrtn

    def test_simple_test(self):
        """Simple test"""
        self.assertFalse(self.launch_client_server())


def main():
    """Simple test runner"""
    suite = unittest.TestLoader().loadTestsFromTestCase(Testnss)
    results = unittest.TestResult()
    suite.run(results)
    print("******************************************************************")
    print("Number test run: %s" % results.testsRun)
    if results.wasSuccessful() is True:
        print("Test was successful\n")
        exit(0)
    else:
        print("Test failed")
    print("\nNumber test errors: %d" % len(results.errors))
    for x in results.errors:
        for l in x:
            print(l)
    print("\nNumber test failures: %d" % len(results.failures))
    for x in results.failures:
        for l in x:
            print(l)
    exit(1)

if __name__ == "__main__":
    main()
