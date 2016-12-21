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
iof cleanup: Terminate the processes and verify the same.

Usage:

Executes from the install/Linux/TESTING directory.
The results are placed in the testLogs/testRun_<date-time-stamp>/
multi_test_nss/cleanup_iof/cleanup_iof_<node> directory.
"""

import unittest
import os
import time
import logging
import shlex
import subprocess

class TestCleanUpIof(unittest.TestCase):
    """Set up and start ctrl fs"""

    startdir = None
    ctrl_dir = None
    logger = logging.getLogger("TestRunnerLogger")

    def setUp(self):
        """Set up the test"""
        self.startdir = os.environ["CNSS_PREFIX"]
        self.ctrl_dir = os.path.join(self.startdir, ".ctrl")

    def launch_cmd(self, msg, cmdstr):
        """Launch a test"""
        self.logger.info("Testnss: start %s - input string:\n %s\n", \
          msg, cmdstr)
        cmdarg = shlex.split(cmdstr)
        procrtn = subprocess.call(cmdarg, timeout=180)
        return procrtn

    def has_terminated(self, proc):
        """Check if the process has terminated
        Wait up to 60s for the process to die by itself,
        return non-zero or zero return code indicating
        success or failure respectively."""
        i = 60
        while i > 0:
            msg = "Check if the %s process has terminated" % proc
            cmd = "pgrep -la %s" % proc
            procrtn = self.launch_cmd(msg, cmd)
            if procrtn is not None:
                break
            else:
                time.sleep(1)
                i = i - 1
        return i


    def test_iofshutdown(self):
        """Shutdown iof"""
        shutdown_file = os.path.join(self.ctrl_dir, "shutdown")
        self.logger.info("Check for shutdown file: %s", shutdown_file)
        os.utime(shutdown_file, None)
        cnssrtn = self.has_terminated("cnss")
        ionssrtn = self.has_terminated("ionss")
        self.logger.info("CNSS %d and IONSS %d", cnssrtn, ionssrtn)
        if cnssrtn != 0 and ionssrtn != 0:
            self.logger.info("CNSS and IONSS processes have terminated.\n")
        else:
            # Log the error message. Fail the test with the same error message
            self.logger.info("IOF Cleanup failed. \
                        CNSS and IONSS processes have not terminated. \
                        CNSS return code: %d. IONSS return code: %d.\n", \
                        cnssrtn, ionssrtn)
            self.fail("IOF Cleanup failed. \
                        CNSS and IONSS processes have not terminated. \
                        CNSS return code: %d. IONSS reutn code: %d." \
                        % (cnssrtn, ionssrtn))
