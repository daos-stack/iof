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
local iof cnss / ionss test

It's possible to launch this test by running:

python3.4 -m unittest -c iof_test_local

Or to launch a specific test by running:

python3.4 -m unittest -c iof_test_local.Testlocal.test_ionss_link

"""

import os
import sys
import time
import stat
import shutil
import tempfile
import logging
import unittest
import common_methods
import iofcommontestsuite

class Testlocal(iofcommontestsuite.CommonTestSuite, common_methods.CnssChecks):
    """Local test"""

    proc = None
    import_dir = None
    export_dir = None
    shutdown_file = None
    e_dir = None

    def is_running(self):
        """Check if the cnss is running"""
        return os.path.exists(self.shutdown_file)

    def setUp(self):
        """set up the test"""

        self.logger.info("Starting for %s", self.id())

        if self.logger.getEffectiveLevel() == logging.WARNING:
            self.logger.setLevel(logging.INFO)
            __ch = logging.StreamHandler(sys.stdout)
            self.logger.addHandler(__ch)

        self.import_dir = tempfile.mkdtemp()
        self.ctrl_dir = os.path.join(self.import_dir, '.ctrl')
        self.shutdown_file = os.path.join(self.ctrl_dir, 'shutdown')
        self.e_dir = tempfile.mkdtemp()

        ompi_bin = os.getenv('IOF_OMPI_BIN', None)

        if ompi_bin:
            orterun = os.path.realpath(os.path.join(ompi_bin, 'orterun'))
        else:
            orterun = 'orterun'

        self.export_dir = os.path.join(self.e_dir, 'exp')
        os.mkdir(self.export_dir)

        log_path = os.getenv("IOF_TESTLOG", 'output/%s' %
                             self.id().split('.')[0])

        # Append the test case to the log directory to get unique names.
        log_path = os.path.join(log_path, self.id().split('.')[2])

        log_mask = os.getenv("CRT_LOG_MASK", "INFO")

        valgrind = iofcommontestsuite.valgrind_suffix()

        cmd = [orterun,
               '--output-filename', log_path,
               '-n', '1',
               '-x', 'CRT_LOG_MASK=%s' % log_mask,
               '-x', 'CNSS_PREFIX=%s' % self.import_dir]
        cmd.extend(valgrind)
        cmd.extend(['cnss',
                    ':',
                    '-n', '1',
                    '-x', 'CRT_LOG_MASK=%s' % log_mask])
        cmd.extend(valgrind)
        cmd.extend(['ionss', self.export_dir, '/usr'])

        self.proc = self.common_launch_process('', ' '.join(cmd))

        elapsed_time = 0
        while not self.is_running():
            elapsed_time += 1
            if elapsed_time > 30:
                self.fail("Could not detect startup in 30 seconds")
            time.sleep(1)

        self.logger.info("Running")

    def tearDown(self):
        """tear down the test"""

        # Firstly try and shutdown the filesystems cleanly
        if self.is_running():
            f = open(self.shutdown_file, 'w')
            f.write('1')
            try:
                f.close()
            except OSError:
                pass

        # Now try to kill the orterun process
        if self.proc is not None:
            procrtn = self.common_stop_process(self.proc)
            if procrtn != 0:
                self.fail("Stopping job returned %d" % procrtn)

        # Call fusermount on mount points in case there are stale mounts.
        # Remove the mount directories
        for mount in ['.ctrl', 'usr', 'exp']:
            mp = os.path.join(self.import_dir, mount)
            self.common_launch_test("", "fusermount -u %s" % mp)
            os.rmdir(mp)

        # Finally, remove any temporary files created.
        os.rmdir(self.import_dir)
        shutil.rmtree(self.export_dir)
        os.rmdir(self.e_dir)

    def test_ionss_link(self):
        """CORFSHIP-336 Check that stat does not deference symlinks"""

        # Make a directory 'b', create a symlink from 'a' to 'b'
        # and then stat() it to see what type it is

        os.mkdir(os.path.join(self.export_dir, 'b'))
        os.symlink('b', os.path.join(self.export_dir, 'a'))

        e_b = os.lstat(os.path.join(self.import_dir, 'exp', 'a'))
        os.unlink(os.path.join(self.export_dir, 'a'))
        os.rmdir(os.path.join(self.export_dir, 'b'))

        self.logger.info(e_b)
        if stat.S_ISLNK(e_b.st_mode):
            self.logger.info("It's a link")
        elif stat.S_ISDIR(e_b.st_mode):
            self.logger.info("It's a dir")
            self.fail("File should be a link")
        else:
            self.fail("Not a directory or a link")

    def test_ionss_self_listdir(self):
        """Perform a simple listdir operation"""

        os.mkdir(os.path.join(self.export_dir, 'test_dir'))

        dirs = os.listdir(os.path.join(self.import_dir, 'exp'))

        self.logger.info(dirs)

        os.rmdir(os.path.join(self.export_dir, 'test_dir'))

    @unittest.skip("Test not complete")
    def test_file_open(self):
        """Open a file for reading

        This is supposed to fail, as the file doesn't exist.
        """

        filename = os.path.join(self.import_dir, 'exp', 'test_file')

        fd = open(filename, 'r')
        fd.close()

    def test_file_open_new(self):
        """Create a new file"""

        filename = os.path.join(self.import_dir, 'exp', 'test_file')

        fd = open(filename, 'w')
        fd.close()

        a = os.stat(filename)
        b = os.stat(os.path.join(self.export_dir, 'test_file'))

        print(a)
        print(b)

        # Currently the FUSE plugin does not correctly report inodes
        # so currently there are differences

        if a != b:
            self.logger.info("File stat data is different")

    def test_many_files(self):
        """Create lots of files, and then perform readdir"""

        test_dir = os.path.join(self.import_dir, 'exp', 'many')

        os.mkdir(os.path.join(self.export_dir, 'many'))


        files = []
        for x in range(0, 100):
            this_file = 'file_%d' % x
            filename = os.path.join(test_dir, this_file)
            fd = open(filename, 'w')
            fd.close()
            files.append(this_file)

        export_list = os.listdir(os.path.join(self.export_dir, 'many'))
        self.logger.info(sorted(files))
        self.logger.info(sorted(export_list))

        import_list = os.listdir(test_dir)
        self.logger.info(sorted(export_list))

        if sorted(files) != sorted(export_list):
            self.fail("Export directory contents are wrong")

        if sorted(files) != sorted(import_list):
            self.fail("Import Directory contents are wrong")

    def test_file_open_existing(self):
        """Open a existing file for reading"""

        tfile = os.path.join(self.export_dir, 'a_file')

        fd = open(tfile, 'w')
        fd.write("Hello")
        fd.close()

        filename = os.path.join(self.import_dir, 'exp', 'a_file')

        fd = open(filename, 'r')
        fd.close()

    def test_file_read(self):
        """Read from a file"""

        tfile = os.path.join(self.export_dir, 'a_file')

        fd = open(tfile, 'w')
        fd.write("Hello")
        fd.close()

        filename = os.path.join(self.import_dir, 'exp', 'a_file')

        fd = open(filename, 'r')
        data = fd.read()
        fd.close()

        if data != 'Hello':
            self.fail('File contents wrong %s %s' % ('Hello', data))
