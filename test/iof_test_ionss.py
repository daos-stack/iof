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
iof cnss / ionss test

Usage:

Execute from the install/Linux/TESTING directory. The results are placed in the
testLogs/nss directory. Any test_ps output is under
<file yaml>_loop#/<module.name.execStrategy.id>/1(process set)/rank<number>.
There you will find anything written to stdout and stderr. The output from
memcheck and callgrind are in the nss directory. At the end of a test run,
the last nss directory is renamed to nss<date stamp>

python3 test_runner srcipts/iof_test_ionss.yml

To use valgrind memory checking
set TR_USE_VALGRIND in iof_test_ionss.yml to memcheck

To use valgrind call (callgrind) profiling
set TR_USE_VALGRIND in iof_test_ionss.yml to callgrind

"""
#pylint: disable=too-many-locals
#pylint: disable=broad-except
import os
import unittest
import shlex
import subprocess
import time
import getpass
import tempfile

fs_list = []

def get_all_cn_cmd():
    """Get prefix to run command to run all all CNs"""
    cn = os.getenv('IOF_TEST_CN')
    if cn:
        return "pdsh -R ssh -w %s " % cn
    return ""

def get_all_ion_cmd():
    """Get prefix to run command on all CNs"""
    ion = os.getenv('IOF_TEST_ION')
    if ion:
        return "pdsh -R ssh -w %s " % ion
    return ""

def manage_ionss_dir():
    """create dirs for IONSS backend"""
    ion_dir = tempfile.mkdtemp()
    os.environ["ION_TEMPDIR"] = ion_dir
    allnode_cmd = get_all_ion_cmd()
    testmsg = "create base ION dirs %s" % ion_dir
    cmdstr = "%smkdir -p %s " % (allnode_cmd, ion_dir)
    launch_test(testmsg, cmdstr)
    i = 2
    while i > 0:
        fs = "FS_%s" % i
        fs_list.append(fs)
        abs_path = os.path.join(ion_dir, fs)
        testmsg = "creating dirs to be used as Filesystem backend"
        cmdstr = "%smkdir -p %s" % (allnode_cmd, abs_path)
        launch_test(testmsg, cmdstr)
        i = i - 1

def setUpModule():
    """ set up test environment """

    print("\nTestnss: module setup begin")
    tempdir = tempfile.mkdtemp()
    os.environ["CNSS_PREFIX"] = tempdir
    allnode_cmd = get_all_cn_cmd()
    testmsg = "create %s on all CNs" % tempdir
    cmdstr = "%smkdir -p %s " % (allnode_cmd, tempdir)
    launch_test(testmsg, cmdstr)
    manage_ionss_dir()
    print("Testnss: module setup end\n\n")

def tearDownModule():
    """teardown module for test"""

    print("Testnss: module tearDown begin")
    testmsg = "terminate any cnss processes"
    cmdstr = "pkill cnss"
    testmsg = "terminate any ionss processes"
    cmdstr = "pkill ionss"
    launch_test(testmsg, cmdstr)
    allnode_cmd = get_all_cn_cmd()
    testmsg = "remove %s on all CNs" % os.environ["CNSS_PREFIX"]
    cmdstr = "%srm -rf %s " % (allnode_cmd, os.environ["CNSS_PREFIX"])
    launch_test(testmsg, cmdstr)
    allnode_cmd = get_all_ion_cmd()
    testmsg = "remove %s on all IONs" % os.environ["ION_TEMPDIR"]
    cmdstr = "%srm -rf %s " % (allnode_cmd, os.environ["ION_TEMPDIR"])
    launch_test(testmsg, cmdstr)

    print("Testnss: module tearDown end\n\n")

def launch_test(msg, cmdstr):
    """Launch a test"""
    print("Testnss: start %s - input string:\n %s\n" % (msg, cmdstr))
    cmdarg = shlex.split(cmdstr)
    procrtn = subprocess.call(cmdarg, timeout=180,
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    return procrtn

def launch_process(msg, cmdstr):
    """Launch a process"""
    print("Testnss: start %s - input string:\n %s\n" % (msg, cmdstr))
    cmdarg = shlex.split(cmdstr)
    proc = subprocess.Popen(cmdarg,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    return proc

def stop_process(msg, proc):
    """wait for processes to terminate"""
    print("Test: %s - stopping processes :%s" % (msg, proc.pid))
    i = 60
    procrtn = None
    while i:
        proc.poll()
        procrtn = proc.returncode
        if procrtn is not None:
            break
        else:
            time.sleep(1)
            i = i - 1

    if procrtn is None:
        procrtn = -1
        try:
            proc.terminate()
            proc.wait(2)
        except ProcessLookupError:
            pass
        except Exception:
            print("Killing processes: %s" % proc.pid)
            proc.kill()

    print("Test: %s - return code: %s\n" % (msg, procrtn))
    return procrtn


def logdir_name(fullname):
    """create the log directory name"""
    names = fullname.split('.')
    items = names[-1].split('_', maxsplit=2)
    return "/" + items[2]

def add_prefix_logdir(testcase_name):
    """add the log directory to the prefix"""
    prefix = ""
    ompi_bin = os.getenv('IOF_OMPI_BIN', "")
    ompi_prefix = os.getenv('IOF_OMPI_PREFIX', "")
    log_path = os.getenv("IOF_TESTLOG", "nss") + logdir_name(testcase_name)
    os.makedirs(log_path, exist_ok=True)
    use_valgrind = os.getenv('TR_USE_VALGRIND', default="")
    if use_valgrind == 'memcheck':
        suppressfile = os.path.join(os.getenv('IOF_MCL_PREFIX', ".."), "etc", \
                       "memcheck-mcl.supp")
        prefix = "valgrind --xml=yes" + \
            " --xml-file=" + log_path + "/valgrind.%q{PMIX_ID}.xml" + \
            " --leak-check=yes --gen-suppressions=all" + \
            " --suppressions=" + suppressfile + " --show-reachable=yes"
    elif use_valgrind == "callgrind":
        prefix = "valgrind --tool=callgrind --callgrind-out-file=" + \
                 log_path + "/callgrind.%q{PMIX_ID}.out"

    if os.getenv('TR_USE_URI', ""):
        dvmfile = " --hnp file:%s " % os.getenv('TR_USE_URI')
    else:
        dvmfile = " "
    if ompi_prefix:
        use_prefix = " --prefix %s" % ompi_prefix
    else:
        use_prefix = ""
    if getpass.getuser() == "root":
        allow_root = " --allow-run-as-root"
    else:
        allow_root = ""
    cmdstr = "%sorterun%s--output-filename %s%s%s" % \
             (ompi_bin, dvmfile, log_path, allow_root, use_prefix)

    return (cmdstr, prefix)

def add_server_client():
    """create the server and client prefix"""
    cn = os.getenv('IOF_TEST_CN')
    if cn:
        local_cn = " -H %s -N 1 " % cn
    else:
        local_cn = " -np 1 "
    ion = os.getenv('IOF_TEST_ION')
    if ion:
        local_ion = " -H %s -N 1 " % ion
    else:
        local_ion = " -np 1"

    return (local_cn, local_ion)


class Testnss(unittest.TestCase):
    """Simple test"""

    proc = None
    def setUp(self):
        print("Testnss: setUp begin")
        print("Testnss: setUp end\n")

    def tearDown(self):
        print("Testnss: tearDown begin")
        testmsg = self.shortDescription()
        procrtn = stop_process(testmsg, self.proc)
        self.assertFalse(procrtn)
        print("Testnss: tearDown end\n\n")

    def test_ionss_simple_test(self):
        """Simple test"""
        testmsg = self.shortDescription()
        (cmd, prefix) = add_prefix_logdir(self.id())
        (cnss, ionss) = add_server_client()
        fs = ' '.join(fs_list)
        test_path = os.getenv('IOF_TEST_BIN', "")
        pass_env = " -x PATH -x LD_LIBRARY_PATH -x CNSS_PREFIX"
        ion_env = " -x ION_TEMPDIR"
        local_server = "%s%s%s %s %s/ionss %s" % \
                       (pass_env, ion_env, ionss, prefix, test_path, fs)
        local_client = "%s%s %s %s/cnss :" % \
                       (pass_env, cnss, prefix, test_path)
        cmdstr = cmd + local_client + local_server
        self.proc = launch_process(testmsg, cmdstr)
        self.assertIsNotNone(self.proc)
        allnode_cmd = get_all_cn_cmd()
        cnss_prefix = os.environ["CNSS_PREFIX"]
        cmdstr = "%s python3.4 %s/testiof.py %s" % \
                 (allnode_cmd, os.path.dirname(os.path.abspath(__file__)), \
                  cnss_prefix)
        launch_test(testmsg, cmdstr)
