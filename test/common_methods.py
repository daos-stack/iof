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
Test methods for creating I/O to a filesystem.

These methods are inhereted by both iof_test_local and iof_simple so the
are invoked on both simple and multi-node launch

"""

import os
import socket
import logging
import tempfile
import unittest

class CnssChecks(unittest.TestCase):
    """A object purely to define test methods.

    These methods are invoked on a node where projections are being imported,
    so it can access the CNSS through the ctrl filesystem.

    There is no side-channel access to the exports however.

    Additionally, for multi-node these tests are executed in parallel so need
    to be carefull to use unique filenames and should endevour to clean up
    after themselves properly.

    This class imports from unittest to get access to the self.fail() method
    """

    ctrl_dir = None
    logger = logging.getLogger("TestRunnerLogger")

    def import_list(self):
        """Return a array of imports

        This list will not be sorted, simply as returned by the kernel.

        For both current users of this file the first entry should be a empty,
        usable mount point.
        """

        imports = []
        entry = os.path.join(self.ctrl_dir, "iof", "projections")
        for projection in os.listdir(entry):
            myfile = os.path.join(entry, projection, 'mount_point')
            fd = open(myfile, "r")
            mnt_path = fd.readline().strip()
            fd.close()
            imports.append(mnt_path)
        return imports

    def get_writeable_import(self):
        """Returns a writeable import directory"""

        return self.import_list()[0]

    @staticmethod
    def get_unique(parent):
        """Return a unique identifer for this user of the projection

        Note that several methods within the same process will get the same
        unique identifier."""

        return tempfile.mkdtemp(dir=parent, prefix='%s_%d_' %
                                (socket.gethostname(), os.getpid()))

    def test_mnt_path(self):
        """Check that mount points do not contain dot characters"""

        for mnt in self.import_list():
            self.assertFalse('.' in mnt, 'mount point should not contain .')

    def test_mkdir(self):
        """Create a directory and check it exists

        This test is also a bit of a nonsense as it makes a directory, however
        it relies on mkdtemp() is get_unique() in order to launch, so if there
        is a problem it'll be the setup which will fail, not the test.
        """

        import_dir = self.get_writeable_import()
        ndir = os.path.join(self.get_unique(import_dir), 'new_dir')

        self.logger.info(self.id())

        self.logger.info("Creating new directory at %s", ndir)

        os.mkdir(ndir)

        if not os.path.isdir(ndir):
            self.fail("Newly created directory does not exist")

        print(os.listdir(ndir))

        os.rmdir(ndir)
