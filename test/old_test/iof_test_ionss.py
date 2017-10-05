#!/usr/bin/env python3
# Copyright (C) 2016-2017 Intel Corporation
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

python3 test_runner scripts/iof_test_ionss.yml

To use valgrind memory checking
set TR_USE_VALGRIND in iof_test_ionss.yml to memcheck

To use valgrind call (callgrind) profiling
set TR_USE_VALGRIND in iof_test_ionss.yml to callgrind

"""

#pylint: disable=too-many-locals
import os
import iofcommontestsuite


class Checknss(iofcommontestsuite.CommonTestSuite):
    """Simple test"""

    proc = None
    def setUp(self):
        """set up the test"""
        self.logger.info("Checknss: setUp begin")
        iofcommontestsuite.commonSetUpModule()
        self.logger.info("Checknss: setUp end\n")

    def tearDown(self):
        """tear down the test"""
        self.logger.info("Checknss: tearDown begin")
        if self.proc is not None:
            procrtn = self.common_stop_process(self.proc)
            if procrtn != 0:
                self.fail("Stopping job returned %d" % procrtn)

        self.commonTearDownModule()
        os.environ.pop("CRT_PHY_ADDR_STR", "")
        os.environ.pop("OFI_INTERFACE", "")
        os.environ.pop("D_LOG_MASK", "")
        self.logger.info("Checknss: tearDown end\n\n")

    def test_ionss_simple_test(self):
        """Simple test"""
        testmsg = self.shortDescription()
        (cmd, prefix) = self.common_add_prefix_logdir()
        (cnss, ionss) = self.common_add_server_client()
        fs = ' '.join(self.fs_list)
        log_mask = os.getenv("D_LOG_MASK", "INFO")
        crt_phy_addr = os.getenv("CRT_PHY_ADDR_STR", "ofi+sockets")
        ofi_interface = os.getenv("OFI_INTERFACE", "eth0")
        test_path = os.getenv('IOF_TEST_BIN', '')
        pass_env = ' -x D_LOG_MASK={!s} -x CRT_PHY_ADDR_STR={!s}' \
                   ' -x OFI_INTERFACE={!s} -x CNSS_PREFIX'.format(
                       log_mask, crt_phy_addr, ofi_interface)

        local_server = "{!s}{!s} {!s} {!s}ionss {!s}".format(
            pass_env, ionss, prefix, test_path, fs)
        local_client = "{!s}{!s} {!s} {!s}cnss :".format(
            pass_env, cnss, prefix, test_path)
        cmdstr = cmd + local_client + local_server
        self.proc = self.common_launch_process(testmsg, cmdstr)
        if self.proc is None:
            self.fail("Unable to launch (c|io)nss")
        allnode_cmd = self.common_get_all_cn_cmd()
        cnss_prefix = os.environ["CNSS_PREFIX"]
        cmdstr = "{!s} python3 {!s}/checkiof.py {!s}".format(
            allnode_cmd, os.path.dirname(os.path.abspath(__file__)),
            cnss_prefix)
        rc = self.common_launch_cmd(testmsg, cmdstr)
        self.logger.info("Proc is %d", rc)
        if rc != 0:
            self.fail("checkiof.py returned %d" % rc)
        rc = self.common_stop_process(self.proc)
        if rc != 0:
            self.fail("Stopping job returned %d" % rc)
        self.proc = None
