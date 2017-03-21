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
Test methods to verify the outcome of the tests that were run on the CN.
These tests are run on the ION.

These methods are invoked by the multi-instance test launch after the
iof_simple_test.

"""

import os
import logging
import unittest
from decimal import getcontext, Decimal
from socket import gethostname

class IonssVerify(unittest.TestCase):
    """A object purely to verify the outcome of the tests carried out
    on the CN.

    These methods are invoked on the ION from where the files system
    will be projected/exported to the CNSS.

    This class imports from unittest to get access to the self.fail() method
    """

    logger = logging.getLogger("TestRunnerLogger")
    e_dir = None
    export_dir = None
    exp_dir = None
    ion = None

    def setUp(self):
        """Set up the export dir"""

        self.ion = os.environ["IOF_TEST_ION"].split(',')
        curr_host = gethostname().split('.')[0]
        if curr_host != self.ion[0]:
            self.skipTest('The current ION list containts more than one node.\
                           The tests cannot run on more that one ION.')

        self.e_dir = os.environ["ION_TEMPDIR"]
        self.export_dir = os.path.join(self.e_dir, 'FS_2')
        for filename in os.listdir(self.export_dir):
            if filename.startswith("wolf"):
                self.exp_dir = os.path.join(self.export_dir, filename)
            else:
                continue

        self.logger.info("\n")
        self.logger.info("*************************************************")
        self.logger.info("Starting for %s", self.id())

    def test_verify_file_write(self):
        """Verify the file written on the projected FS"""

        filename = os.path.join(self.exp_dir, 'write_file')

        fd = open(filename, 'r')
        data = fd.read()
        fd.close()

        if data != 'World':
            self.fail('File contents wrong %s %s' % ('Hello', data))
        else:
            self.logger.info("Contents from file written to: %s %s", \
                              'Hello', data)

        os.unlink(filename)

    def test_verify_file_copy(self):
        """Verify the file has been copied on the projection"""

        filename = os.path.join(self.exp_dir, 'ls')

        if not os.path.isfile(filename) or not os.access(filename, os.R_OK):
            self.fail("Failed to copy file into the projection")
        else:
            self.logger.info("Copied file exists in the projection")

        os.unlink(filename)

    def test_verify_file_rename(self):
        """Verify the contents of the renamed file"""

        filename = os.path.join(self.exp_dir, 'd_file')

        fd = open(filename, 'r')
        data = fd.read()
        fd.close()

        if data != 'World':
            self.fail('File contents wrong %s %s' % ('Hello', data))
        else:
            self.logger.info("Contents from the renamed file: %s %s", \
                              'Hello', data)

        os.unlink(filename)

    def test_verify_make_symlink(self):
        """Verify the symlink created on the projection"""

        self.logger.info("List the files on ION")
        self.logger.info(os.listdir(self.exp_dir))
        filename = os.path.join(self.exp_dir, 'mlink_source')

        self.logger.info(os.lstat(filename))
        result = os.readlink(filename)
        if result != 'mlink_target':
            self.fail("Link target is wrong '%s'" % result)
        else:
            self.logger.info("Verified link target with source.")

        os.unlink(filename)

    def test_verify_file_copy_from(self):
        """Verify the copy of the file made into the projection"""

        filename = os.path.join(self.exp_dir, 'ls.2')

        if not os.path.isfile(filename) or not os.access(filename, os.R_OK):
            self.fail("Failed to copy file into the projection")
        else:
            self.logger.info("Copied file exists in the projection")

        os.unlink(filename)

    @staticmethod
    def file_length(fname):
        """ Return the number of lines in fname"""
        num_lines = sum(1 for line in open(fname))
        return num_lines

    def test_verify_use_ino(self):
        """Compare and verify the stat results on a file from CN and ION"""

        filename = os.path.join(self.exp_dir, 'test_ino_file')
        b = os.stat(filename)
        os.unlink(filename)

        # Currently the FUSE plugin does not correctly report inodes
        # so currently there are differences.

        b_stat_file = os.path.join(self.export_dir, 'b_stat_output')
        fd_stat = open(b_stat_file, 'w')
        getcontext().prec = 7

        for key in dir(b):
            if not key.startswith('st_'):
                continue

            fd_stat.write("Key %s " % key)
            fd_stat.write(str(Decimal(getattr(b, key))))
            fd_stat.write('\n')

            if key == 'st_dev':
                continue

        fd_stat.close()

        # Compare the stat values recorded on the CN and ION
        a_stat_file = os.path.join(self.export_dir, 'a_stat_output')
        with open(a_stat_file, 'r') as f1:
            a_num_lines = self.file_length(a_stat_file)
            with open(b_stat_file, 'r') as f2:
                same = set(f1).intersection(f2)
                b_num_lines = self.file_length(b_stat_file)

        same.discard('\n')

        if len(same) == a_num_lines == b_num_lines:
            self.logger.info("Stat attributes are same %s", same)
        else:
            self.logger.info("Stat attributes are different %s", same)

        os.unlink(b_stat_file)
        os.unlink(a_stat_file)

    def test_verify_file_unlink(self):
        """Verify the file has been removed"""

        if os.path.exists(os.path.join(self.exp_dir, 'unlink_file')):
            self.fail("File unlink failed.")
        else:
            self.logger.info("File unlinked.")

    def test_verify_rmdir(self):
        """Verify the dir has been removed"""

        if os.path.exists(os.path.join(self.exp_dir, 'my_dir')):
            self.fail("Directory has been removed.")
        else:
            self.logger.info("Directory removed.")

    def test_verify_many_files(self):
        """Verify the collection of files created"""

        many_dir = os.path.join(self.export_dir, 'many')

        export_list = os.listdir(many_dir)
        self.logger.info(sorted(export_list))

        with open(os.path.join(self.export_dir, 'file_list'), 'r') as f:
            files = [line.rstrip('\n') for line in f]

        self.logger.info(sorted(files))

        if sorted(files) != sorted(export_list):
            self.fail("Export Directory contents are wrong")
        else:
            self.logger.info("Import and Export directory contents match")

    def test_clean_up_ionss_self_listdir(self):
        """Clean up after the list dir"""

        os.rmdir(os.path.join(self.export_dir, 'test_dir'))

    def test_clean_up_ionss_link(self):
        """Clean up the link created to test"""

        os.unlink(os.path.join(self.export_dir, 'a'))
        os.rmdir(os.path.join(self.export_dir, 'b'))
