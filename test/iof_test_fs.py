#!/usr/bin/env python3
# Copyright (C) 2016 Intel Corporation
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted for any purpose (including commercial purposes)
# provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions, and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions, and the following disclaimer in the
#    documentation and/or materials provided with the distribution.
#
# 3. In addition, redistributions of modified forms of the source or binary
#    code must carry prominent notices stating that the original code was
#    changed and the date of the change.
#
#  4. All publications or advertising materials mentioning features or use of
#     this software are asked, but not required, to acknowledge that it was
#     developed by Intel Corporation and credit the contributors.
#
# 5. Neither the name of Intel Corporation, nor the name of any Contributor
#    may be used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
# DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
# THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
"""
iof fs test

Usage:

Execute from the install/$arch/TESTING directory. The results are placed in the
testLogs/testrun/test_fs directory. Any test_ps output is under
<file yaml>_loop#/<module.name.execStrategy.id>/1(process set)/rank<number>.
There you will find anything written to stdout and stderr. The output from
memcheck and callgrind are in the test_fs directory. At the end of a test run,
the last test_fs directory is renamed to test_fs<date stamp>

python3 test_runner srcipts/iof_test_fs.yml

To use valgrind memory checking
set TR_USE_VALGRIND in iof_test_fs.yml to memcheck

To use valgrind call (callgrind) profiling
set TR_USE_VALGRIND in iof_test_fs.yml to callgrind

"""

#pylint: disable=too-few-public-methods
#pylint: disable=broad-except

import unittest
import os
import sys
import shutil
import shlex
import subprocess
import time

PREFIX = ""
TESTLOG = ""

def setUpModule():
    """setup module for fs test"""
    global PREFIX
    global TESTLOG
    print("TestIof: module setUp begin")
    TESTLOG = os.getenv("IOF_TESTLOG", "test_fs") + "/fs_test"
    os.makedirs(TESTLOG, exist_ok=True)
    use_valgrind = os.getenv("TR_USE_VALGRIND")
    print("Setting up for valgrind: %s" % use_valgrind)
    if use_valgrind == "memcheck":
        suppressfile = os.path.join(os.getenv('IOF_MCL_PATH', ".."), "etc", \
                       "memcheck-mcl.supp")
        PREFIX = "valgrind --xml=yes" + \
            " --xml-file=" + TESTLOG + "/valgrind.%q{PMIX_ID}.xml" + \
            " --leak-check=yes --gen-suppressions=all" + \
            " --suppressions=" + suppressfile + " --show-reachable=yes"
    elif use_valgrind == "callgrind":
        PREFIX = "valgrind --tool=callgrind --callgrind-out-file=" + \
                 TESTLOG + "/callgrind.%q{PMIX_ID}.out"
    else:
        PREFIX = ""

    print("TestIof: module setup end\n")

def tearDownModule():
    """ teardown module """
    print("TestIof: module tearDown begin")
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
        ompi_path = os.getenv('MCL_OMPI_PATH', "")
        server_list = os.getenv('IOF_TEST_ION', "")
        client_list = os.getenv('IOF_TEST_CN', "")
        if server_list:
            servers = " -H %s " % server_list
        else:
            servers = " "
        if client_list:
            clients = " -H %s " % client_list
        else:
            clients = " "
        print("TestIof: Setting up mount dir for fs test.")
        self.startdir = os.getcwd()
        self.mnt = os.getenv("IOF_MOUNT_DIR", "testDir/child_fs")
        mnt = self.mnt
        if os.path.exists(mnt):
            shutil.rmtree(mnt)
        os.makedirs(mnt)
        cmd = "%sorterun --output-filename %s" \
              "%s-np 1 %s tests/client_main -mnt %s :" \
              "%s-np %d %s tests/server_main" % \
              (ompi_path, TESTLOG, clients, PREFIX, mnt, \
               servers, self.number_servers, PREFIX)
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
        if os.getenv("TR_USE_VALGRIND") == "memcheck":
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

    def test_iof_fs_test(self):
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
