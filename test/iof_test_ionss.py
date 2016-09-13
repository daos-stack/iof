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

def setUpModule():
    """ set up test environment """

    print("\nTestnss: module setup begin")
    print("Testnss: module setup end\n\n")

def tearDownModule():
    """teardown module for test"""
    print("Testnss: module tearDown begin")
    testmsg = "terminate any cnss processes"
    cmdstr = "pkill cnss"
    launch_test(testmsg, cmdstr)
    testmsg = "terminate any ionss processes"
    cmdstr = "pkill ionss"
    launch_test(testmsg, cmdstr)
    print("Testnss: module tearDown end\n\n")

def launch_test(msg, cmdstr):
    """Launch process set test"""
    print("Testnss: start %s - input string:\n %s\n" % (msg, cmdstr))
    cmdarg = shlex.split(cmdstr)
    start_time = time.time()
    procrtn = subprocess.call(cmdarg, timeout=180,
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
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
    ompi_path = os.getenv('IOF_OMPI_PATH', "")
    ompi_prefix = os.getenv('IOF_OMPI_PREFIX', "")
    log_path = os.getenv("IOF_TESTLOG", "nss") + logdir_name(testcase_name)
    os.makedirs(log_path, exist_ok=True)
    use_valgrind = os.getenv('TR_USE_VALGRIND', default="")
    if use_valgrind == 'memcheck':
        suppressfile = os.path.join(os.getenv('IOF_MCL_PATH', ".."), "etc", \
                       "memcheck-mcl.supp")
        prefix = "valgrind --xml=yes" + \
            " --xml-file=" + log_path + "/valgrind.%q{PMIX_ID}.xml" + \
            " --leak-check=yes --gen-suppressions=all" + \
            " --suppressions=" + suppressfile + " --show-reachable=yes"
    elif use_valgrind == "callgrind":
        prefix = "valgrind --tool=callgrind --callgrind-out-file=" + \
                 log_path + "/callgrind.%q{PMIX_ID}.out"

    if os.path.exists("./orted-uri"):
        dvmfile = " --hnp file:orted-uri "
    else:
        dvmfile = " "
    if ompi_prefix:
        use_prefix = " --prefix %s" % ompi_prefix
    else:
        use_prefix = ""
    cmdstr = "%sorterun%s--output-filename %s%s" % \
             (ompi_path, dvmfile, log_path, use_prefix)

    return (cmdstr, prefix)

def add_server_client():
    """create the server and client prefix"""
    cn = os.getenv('IOF_TEST_CN')
    if cn:
        local_cn = " -H %s " % cn
    else:
        local_cn = " "
    ion = os.getenv('IOF_TEST_ION')
    if ion:
        local_ion = " -H %s " % ion
    else:
        local_ion = " "

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
        local_server = "%s-np 4 %s ionss :" % (ionss, prefix)
        local_client = "%s-np 3 %s cnss" % (cnss, prefix)
        cmdstr = cmd + local_server + local_client
        self.assertFalse(launch_test(testmsg, cmdstr))
