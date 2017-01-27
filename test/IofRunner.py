#!/usr/bin/env python3
# Copyright (c) 2016-2017 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
# -*- coding: utf-8 -*-
"""
Iofrunner class
The  Iofrunner is triggered by the TestRunner to:
Create the CNSS and IONSS dirs
Trigger the CNSS and IONSS process
and post test run to:
Terminate the CNSS and IONSS processes
Unmount and remove the CNSS and IONSS dirs
"""

import os
import subprocess
import shlex
import logging
import getpass
import tempfile

#pylint: disable=broad-except
#pylint: disable=too-many-instance-attributes

class IofRunner():
    """Simple test runner"""
    fs_list = []

    def __init__(self, dir_path, test_info, node_control):
        self.dir_path = dir_path
        self.test_info = test_info
        self.node_control = node_control
        self.logger = logging.getLogger("TestRunnerLogger")
        self.proc = None
        self.logfileout = ""
        self.logfileerr = ""

    def create_cnss_dir(self):
        """Create the CNSS dir on all CNs"""
        procrtn = None
        testmsg = "Create the CNSS dir on all CNs"
        tempdir = tempfile.mkdtemp()
        self.test_info.set_passToConfig('CNSS_PREFIX', tempdir)
        os.environ['CNSS_PREFIX'] = tempdir
        cmdstr = "mkdir -p %s " % tempdir
        procrtn = self.node_control.execute_cmd(cmdstr, self.dir_path,
                                                'IOF_TEST_CN', testmsg)
        if procrtn:
            self.logger.error(
                "TestIOF: Failed to create the CNSS dirs. \
                 Error code: %d\n", procrtn)

    def manage_ionss_dir(self):
        """Create dirs for IONSS backend"""
        procrtn = None
        testmsg = "create dirs for IONSS backend"
        ion_dir = tempfile.mkdtemp()
        self.test_info.set_passToConfig('ION_TEMPDIR', ion_dir)
        os.environ['ION_TEMPDIR'] = ion_dir
        cmdstr = "mkdir -p %s " % ion_dir
        procrtn = self.node_control.execute_cmd(cmdstr, self.dir_path,
                                                'IOF_TEST_ION', testmsg)
        if procrtn:
            self.logger.error(
                "TestIOF: Failed to create the IONSS dirs. Error code: %d\n",
                procrtn)
        for i in (2, 1):
            fs = "FS_%s" % i
            self.fs_list.append(fs)
            abs_path = os.path.join(ion_dir, fs)
            testmsg = "creating dirs to be used as Filesystem backend"
            cmdstr = "mkdir -p %s" % abs_path
            procrtn = self.node_control.execute_cmd(cmdstr, self.dir_path,
                                                    'IOF_TEST_ION', testmsg)
            if procrtn:
                self.logger.error(
                    """"TestIOF: Failed to create dirs for Filesystem
                     backend %s. Error code: %d\n""", fs, procrtn)

    def add_prefix_logdir(self):
        """Add the log directory to the prefix"""
        ompi_bin = self.test_info.get_defaultENV('IOF_OMPI_BIN')
        log_path = os.path.join(self.dir_path, "ionss")
        os.makedirs(log_path, exist_ok=True)
        if self.test_info.get_defaultENV('TR_USE_URI'):
            dvmfile = " --hnp file:%s " % \
                      self.test_info.get_defaultENV('TR_USE_URI')
        else:
            dvmfile = " "
        if getpass.getuser() == "root":
            allow_root = " --allow-run-as-root"
        else:
            allow_root = ""
        cmdstr = "%sorterun%s--output-filename %s%s" % \
                 (ompi_bin, dvmfile, log_path, allow_root)

        return cmdstr

    def add_server(self):
        """Create the server prefix"""
        ion = self.test_info.get_defaultENV('IOF_TEST_ION')
        if ion:
            local_ion = " -H %s -N 1 " % (ion)
        else:
            local_ion = " -np 1"

        return local_ion

    def add_client(self):
        """Create the client prefix"""
        cn = self.test_info.get_defaultENV('IOF_TEST_CN')
        if cn:
            local_cn = " -H %s -N 1 " % (cn)
        else:
            local_cn = " -np 1 "

        return local_cn

    def setup_env(self):
        """setup environment variablies"""
        cci_path = self.test_info.get_defaultENV('CCI_CONFIG')
        os.environ['CCI_CONFIG'] = cci_path
        cart_log_mask = self.test_info.get_defaultENV('CRT_LOG_MASK',
                                                      "DEBUG,MEM=ERR")
        os.environ['CRT_LOG_MASK'] = cart_log_mask

    def launch_process(self):
        """Launch the CNSS and IONSS processes"""
        self.logger.info("Testnss: Launch the CNSS and IONSS processes")
        pass_env = " -x CCI_CONFIG -x CNSS_PREFIX -x CRT_LOG_MASK"
        self.setup_env()
        self.proc = None
        self.create_cnss_dir()
        self.manage_ionss_dir()
        cmd = self.add_prefix_logdir()
        ionss = self.add_server()
        cnss = self.add_client()
        test_path = self.test_info.get_defaultENV('IOF_TEST_BIN')
        fs = ' '.join(self.fs_list)
        ion_env = " -x ION_TEMPDIR"
        local_server = "%s%s%s %s/ionss %s" % \
                        (pass_env, ion_env, ionss, test_path, fs)
        local_client = "%s%s %s/cnss :" % \
                        (pass_env, cnss, test_path)
        cmdstr = cmd + local_client + local_server
        self.logger.info("Testionss: %s", cmdstr)
        self.logfileout = os.path.join(self.dir_path, "iofRunner.out")
        self.logfileerr = os.path.join(self.dir_path, "iofRunner.err")
        cmdarg = shlex.split(cmdstr)
        with open(self.logfileout, mode='w+b', buffering=0) as outfile, \
            open(self.logfileerr, mode='w+b', buffering=0) as errfile:
            self.proc = subprocess.Popen(cmdarg,
                                         stdin=subprocess.DEVNULL,
                                         stdout=outfile,
                                         stderr=errfile)
        return not self.proc

    def stop_process(self):
        """Remove temporary dirs and stale mountpoints.
        Wait for processes to terminate and terminate them after
        the wait period."""
        self.logger.info("Testionss: - stopping processes :%s", self.proc.pid)
        #self.remove_cnss_ionss_dir()
        self.stop_cnss_ionss_processes("ionss", "IOF_TEST_ION")
        self.proc.poll()
        procrtn = self.proc.returncode
        if procrtn is None:
            procrtn = -1
            try:
                self.proc.terminate()
                self.proc.wait(2)
                procrtn = self.proc.returncode
            except ProcessLookupError:
                pass
            except Exception:
                self.logger.error("Killing processes: %s", self.proc.pid)
                self.proc.kill()

        self.remove_cnss_ionss_dir()
        self.logger.info("Testionss: - return code: %s\n", procrtn)
        return procrtn

    def stop_cnss_ionss_processes(self, proc_name, node_type):
        """Kill the remote CNSS process and IONSS processes in case the
        test's cleanup failed to kill them. The node_control executes the
        command on the remote nodes specified by the node_type."""
        testmsg = "terminate any %s processes" % proc_name
        cmdstr = "pkill %s" % proc_name
        self.node_control.execute_cmd(cmdstr, self.dir_path, node_type, testmsg)
        testmsg = "check for any %s processes" % proc_name
        cmdstr = "pgrep -la %s" % proc_name
        procrtn = self.node_control.execute_cmd(cmdstr, self.dir_path,
                                                node_type, testmsg)
        if not procrtn:
            self.logger.error(
                "TestIOF: Failed to kill the %s process. \
                 Error code: %d ", proc_name, procrtn)

    def remove_cnss_ionss_dir(self):
        """ Call fusermount to unmount the stale mounts.
        Remove the temporary files created during setup."""
        procrtn = None
        cnss_dir = self.test_info.get_passToConfig('CNSS_PREFIX')
        ionss_dir = self.test_info.get_passToConfig('ION_TEMPDIR')
        cnss_mp = os.path.join(cnss_dir, ".ctrl")
        testmsg = "Unmount CNSS dirs"
        cmdstr = "fusermount -u %s" % cnss_mp
        procrtn = self.node_control.execute_cmd(cmdstr, self.dir_path,
                                                'IOF_TEST_CN', testmsg)
        testmsg = "Remove CNSS dirs"
        cmdstr = "rm -rf " + cnss_dir
        procrtn = self.node_control.execute_cmd(cmdstr, self.dir_path,
                                                'IOF_TEST_CN', testmsg)
        if procrtn:
            self.logger.error(
                "TestIOF: Failed to remove CNSS dirs. Error code: %d", procrtn)
        testmsg = "Remove IONSS dirs"
        cmdstr = "rm -rf " + ionss_dir
        procrtn = self.node_control.execute_cmd(cmdstr, self.dir_path,
                                                'IOF_TEST_ION', testmsg)
        if procrtn:
            self.logger.error(
                "TestIOF: Failed to remove IONSS dirs. Error code: %d", procrtn)
