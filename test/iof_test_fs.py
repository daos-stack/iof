#!/usr/bin/env python3
"""
iof fs test

Usage:

Should be executed from the iof root directory. The results are placed in the
testLogs/test_fs directory. Any test_ps output is under
testLogs/test_fs/1(process set)/rank<number>. There you will find anything
written to stdout and stderr. The output from memcheck and callgrind are in
the test_fs directory. At the end of a test run, the last test_fs directory is
renamed to test_fs<date stamp>

python3 test/iof_test_fs.py [options]

Options:
memcheck  - To use valgrind memory checking

callgrind - To use valgrind call (callgrind) profiling

servers=<number> - set the number of servers, default: 2

ckients=<number> - set the number of clients, default: 1

mountDir=<dir> - change the default mount point, default: "testDir/child_fs"

"""

#pylint: disable=too-many-locals
#pylint: disable=too-many-branches
#pylint: disable=too-many-statements
#pylint: disable=too-few-public-methods
#pylint: disable=broad-except

import unittest
import os
import sys
import shutil
import shlex
import subprocess
import time
import json
from datetime import datetime

PREFIX = ''

def setUpModule():
    """ set up test environment """
    global PREFIX
    info = {}
    print("\nTestIof: module setup begin")
    rootpath = os.getcwd()
    print("TestIof path: %s" % rootpath)
    print("TestIof input: %s" % sys.argv[0])
    where = os.path.dirname(sys.argv[0])
    print("TestIof input file: %s" % (where))
    if not where:
        os.chdir("..")
        rootpath = os.getcwd()
    platform = os.uname()[0]
    opts_file = rootpath + "/.build_vars-%s.py" % platform
    print("TestIof use file: %s" % opts_file)
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
    if  platform == "Darwin":
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
        for k in range(1, len(sys.argv)):
            arg = sys.argv[k].split('=', 1)
            #print ("arg: %s" % arg)
            if arg[0] == "memcheck":
                os.environ['MCL_USE_VALGRIND'] = "memcheck"
                PREFIX = "valgrind --xml=yes " + \
                    "--xml-file=testLogs/test_fs/valgrind.%q{PMIX_ID}.xml" + \
                    " --leak-check=yes --show-reachable=yes" + \
                    " --suppressions=" + info['MCL_PREFIX'] + \
                    "/etc/memcheck-mcl.supp"
            if arg[0] == "callgrind":
                os.environ['MCL_USE_VALGRIND'] = "callgrind"
                PREFIX = "valgrind --tool=callgrind " + \
                    "--callgrind-out-file=" + \
                    "testLogs/test_fs/callgrind.%q{PMIX_ID}.out"
            if arg[0] == "servers":
                print("TestIof: number servers: %s" % arg[1])
                os.environ['IOF_USE_SERVERS'] = arg[1]
            if arg[0] == "mountDir":
                print("TestIof: mount directory: %s" % arg[1])
                os.environ['IOF_MOUNT_DIR'] = arg[1]
            if arg[0] == "clients":
                print("TestIof: number clients: %s" % arg[1])
                os.environ['IOF_USE_CLIENTS'] = arg[1]
    else:
        PREFIX = ""
    if os.path.exists("testLogs/test_fs"):
        newname = "testLogs/test_fs_%s" % datetime.now().isoformat()
        os.rename("testLogs/test_fs", newname)
    os.makedirs("testLogs/test_fs")

    print("TestIof: module setup end\n\n")

def tearDownModule():
    """ If callgrind is used pull in the results """
    print("TestIof: module tearDown begin")
    if os.getenv("MCL_USE_VALGRIND") == "callgrind":
        print("TestIof: Annotating the Callgrind out files")
        srcfile = " %s/proto/iof_prototype/*.c" % os.getcwd()
        os.chdir("testLogs/test_fs")
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

    if os.path.exists("testLogs/test_fs"):
        newname = "testLogs/test_fs_%s" % datetime.now().isoformat()
        os.rename("testLogs/test_fs", newname)
    print("TestIof: module tearDown end\n\n")

class TestIof():
    """ class to test the mount point """

    mnt = ""
    startdir = ""

    def __init__(self, mount, home):
        self.mnt = mount
        self.homedir = home

    def simpleTest(self, rank):
        """ test the mount point """
        print("\nTestIof: check file Rank%d" % rank)
        mnt = self.mnt + "/Rank" + str(rank)
        print("TestIof: change working directory to: %s" % mnt)
        os.chdir(mnt)
        print("TestIof: mkdir d & e")
        # need to cleanup from last failure, if file exists
        os.mkdir("d")
        os.mkdir("e")
        print("TestIof: check mkdir d & e")
        dirlist = os.listdir('.')
        dirlist.index("d")
        dirlist.index("e")

        print("TestIof: check rmdir e")
        os.rmdir("e")
        if os.path.exists("e"):
            tb = sys.exc_info()[2]
            raise AssertionError("Deletion was unsuccessful").with_traceback(tb)

        print("TestIof: check symlink d -> d_sym")
        os.symlink("d", "d_sym")
        if os.path.islink("d_sym") is False:
            tb = sys.exc_info()[2]
            raise AssertionError("link not found").with_traceback(tb)

        print("TestIof: check broken symlink ops")
        os.rmdir("d")
        if os.path.islink("d_sym") is False:
            tb = sys.exc_info()[2]
            raise AssertionError("link not found").with_traceback(tb)
        os.remove("d_sym")
        if os.path.exists("d_sym"):
            tb = sys.exc_info()[2]
            raise AssertionError("link not remoted").with_traceback(tb)

        print("TestIof: create symlink target origin")
        os.symlink("target", "origin")
        print("TestIof: read symlink origin")
        linkname = os.readlink("origin")
        os.chdir(self.homedir)
        return linkname

    def simpleUserExit(self, rank):
        """ test the mount point """
        print("\nTestIof: check user exit directory Rank%d" % rank)
        mnt = self.mnt + "/Rank" + str(rank)
        platform = os.uname()[0]
        if  platform != "Darwin":
            cmd = ["setfattr", "-n", "user.exit", "-v", "1", mnt]
        else:
            cmd = ["xattr", "-w", "user.exit", "1", mnt]
        print("TestIof: %s" % cmd)
        subprocess.call(cmd, timeout=30)

class TestIofMain(unittest.TestCase):
    """ Main iof test """
    proc = None
    # mnt is set to testDir/child_fs by default when IOF_MOUNT_DIR is read
    mnt = ""
    number_clients = 1
    number_servers = 2
    startdir = ""

    def setUp(self):
        print("TestIof setUp begin")
        print("TestIof: Setting up for fs test.")
        self.number_servers = int(os.getenv("IOF_USE_SERVERS", "2"))
        self.number_clients = int(os.getenv("IOF_USE_CLIENTS", "1"))
        print("TestIof: Setting up mount dir for fs test.")
        self.startdir = os.getcwd()
        self.mnt = os.getenv("IOF_MOUNT_DIR", "testDir/child_fs")
        mnt = self.mnt
        if os.path.exists(mnt):
            shutil.rmtree(mnt)
        os.makedirs(mnt)
        cmd = "orterun --output-filename testLogs/test_fs" + \
              " -np 1 %s client_main -mnt %s :"  % (PREFIX, mnt) + \
              " -np %d %s server_main" % (self.number_servers, PREFIX)
        print("TestIof: start processes")
        cmdarg = shlex.split(cmd)
        print("TestIof: input string: %s\n" % cmd)
        localproc = subprocess.Popen(cmdarg,
                                     stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        self.assertIsNotNone(localproc)
        print("TestIof:pid: %d" % (localproc.pid))
        self.proc = localproc
        print("TestIof setUp end\n")
        return 0

    def tearDown(self):
        print("\nTestIof tearDown begin")
        print("TestIof: Tearing down environment for fs test.")
        mnt = self.mnt
        os.chdir(self.startdir)
        self.proc.poll()
        ismounted = []
        if self.proc.returncode is None:
            for k in range(self.number_servers):
                directory = "%s/Rank%d" % (mnt, k)
                print("TestIof: check mounted file system %s" % directory)
                ismounted.append(os.path.ismount(str(directory)))
            print("TestIof: Stopping processes :%s" % self.proc.pid)
            self.proc.terminate()
            try:
                self.proc.wait(2)
            except subprocess.TimeoutExpired:
                pass

        platform = os.uname()[0]
        if  platform != "Darwin":
            umount_cmd = ['fusermount', '-u']
        else:
            umount_cmd = ['umount']
        for k in range(self.number_servers):
            directory = "%s/Rank%d" % (mnt, k)
            umount_cmd.append(str(directory))
            if ismounted[k]:
                print("TestIof: unmounting file system %s" % directory)
                print("TestIof: %s" % umount_cmd)
                subprocess.call(umount_cmd, timeout=30)
            umount_cmd.remove(str(directory))
        self.proc.poll()
        if self.proc.returncode is None:
            print("TestIof: Again stopping processes :%s" % self.proc.pid)
            try:
                self.proc.terminate()
                self.proc.wait(2)
            except ProcessLookupError:
                pass
            except Exception:
                print("TestIof: killing processes :%s" % self.proc.pid)
                self.proc.kill()

        # remove the mount points
        if os.path.isdir(mnt):
            cmd = ["rm", "-rf", mnt]
            subprocess.call(cmd, timeout=30)
        print("TestIof tearDown end\n\n")
        return 0

    def simpleWait(self):
        """Wait for processes to start"""
        if os.getenv("MCL_USE_VALGRIND") == "memcheck":
            print("TestIof: waiting for valgrind to start")
            time.sleep(20)
        i = 10
        servers = self.number_servers
        serverlist = list(range(servers))
        mnt = self.mnt
        while (servers > 0) and (i > 0):
            time.sleep(1)
            i = i - 1
            for k in serverlist:
                filename = "%s/Rank%d/started" % (mnt, k)
                if os.path.exists(filename) is True:
                    print("TestIof: Rank %d started file: %s" % (k, filename))
                    serverlist.remove(k)
                    servers = servers - 1
        print("TestIof: check for started loop %d" % i)
        return i

    def testSimpleIof(self):
        """Simple fs"""
        self.assertTrue(self.simpleWait())

        child = TestIof(self.mnt, self.startdir)
        for k in range(self.number_servers):
            try:
                linkname = child.simpleTest(k)
            except Exception as e:
                print("TestIof: test failed %s", e)
                return 1
            self.assertEqual(linkname, 'target')

        for k in range(self.number_servers):
            child.simpleUserExit(k)

def main():
    """Simple test runner"""
    suite = unittest.TestLoader().loadTestsFromTestCase(TestIofMain)
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
