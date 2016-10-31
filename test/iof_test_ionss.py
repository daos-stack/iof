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

import os
import unittest
import shlex
import subprocess
import time
import getpass
import tempfile

def get_all_node_cmd():
    """Get prefix to run command to run all all CNs"""
    cn = os.getenv('IOF_TEST_CN')
    if cn:
        return "pdsh -R ssh -w %s " % cn
    return ""

def setUpModule():
    """ set up test environment """

    print("\nTestnss: module setup begin")
    tempdir = tempfile.mkdtemp()
    os.environ["CNSS_PREFIX"] = tempdir
    allnode_cmd = get_all_node_cmd()
    testmsg = "create %s on all CNs" % tempdir
    cmdstr = "%smkdir -p %s " % (allnode_cmd, tempdir)
    launch_test(testmsg, cmdstr)
    print("Testnss: module setup end\n\n")

def tearDownModule():
    """teardown module for test"""

    print("Testnss: module tearDown begin")
    testmsg = "terminate any cnss processes"
    cmdstr = "pkill cnss"
    testmsg = "terminate any ionss processes"
    cmdstr = "pkill ionss"
    launch_test(testmsg, cmdstr)
    allnode_cmd = get_all_node_cmd()
    testmsg = "remove %s on all CNs" % os.environ["CNSS_PREFIX"]
    cmdstr = "%srm -rf %s " % (allnode_cmd, os.environ["CNSS_PREFIX"])
    launch_test(testmsg, cmdstr)

    print("Testnss: module tearDown end\n\n")

def launch_test(msg, cmdstr, cnss=False):
    """Launch process set test"""
    print("Testnss: start %s - input string:\n %s\n" % (msg, cmdstr))
    cmdarg = shlex.split(cmdstr)
    start_time = time.time()
    proc = subprocess.Popen(cmdarg,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    procrtn = 0
    if cnss:
        testmsg = "Tell CNs to shutdown"
        shutdown = os.path.join(os.getcwd(), "scripts/signal_shutdown.sh")
        subcmd = "%s %s" % (shutdown, os.environ["CNSS_PREFIX"])
        allnode_cmd = get_all_node_cmd()
        cmdstr = "%s%s" % (allnode_cmd, subcmd)
        print("Execute %s" % cmdstr)
        procrtn = launch_test(testmsg, cmdstr)
    if procrtn != 0:
        print("Failed to initiate shutdown.  Failing test")
    else:
        procrtn = proc.wait(timeout=180)
    elapsed = time.time() - start_time
    print("Testnss: %s - return code: %d test duration: %d\n" %
          (msg, procrtn, elapsed))
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

    def setUp(self):
        print("Testnss: setUp begin")
        print("Testnss: setUp end\n")

    def tearDown(self):
        print("Testnss: tearDown begin")
        print("Testnss: tearDown end\n\n")

    def test_ionss_simple_test(self):
        """Simple test"""
        testmsg = self.shortDescription()
        (cmd, prefix) = add_prefix_logdir(self.id())
        (cnss, ionss) = add_server_client()
        test_path = os.getenv('IOF_TEST_BIN', "")
        pass_env = " -x PATH -x LD_LIBRARY_PATH -x CNSS_PREFIX"
        local_server = "%s%s %s %s/ionss :" % \
                       (pass_env, ionss, prefix, test_path)
        local_client = "%s%s %s %s/cnss" % \
                       (pass_env, cnss, prefix, test_path)
        cmdstr = cmd + local_server + local_client
        self.assertFalse(launch_test(testmsg, cmdstr, cnss=True))
