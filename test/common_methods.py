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

These methods are inhereted by both iof_test_local and iof_simple_test so the
are invoked on both simple and multi-node launch

"""

import os
import subprocess
import logging
import tempfile
import unittest
import stat
import shutil
import json
from socket import gethostname
from decimal import Decimal
import time
import iof_ionss_setup
import iof_ionss_verify
#pylint: disable=import-error
#pylint: disable=no-name-in-module
from distutils.spawn import find_executable
#pylint: enable=import-error
#pylint: enable=no-name-in-module


IMPORT_MNT = None
CTRL_DIR = None


def import_list():
    """Return a array of imports

    This list will not be sorted, simply as returned by the kernel.

    For both current users of this file the first entry should be a empty,
    usable mount point.
    """

    imports = []
    entry = os.path.join(CTRL_DIR, "iof", "projections")
    for projection in os.listdir(entry):
        myfile = os.path.join(entry, projection, 'mount_point')
        with open(myfile, "r") as fd:
            mnt_path = fd.readline().strip()
        fd.close()
        imports.append(mnt_path)
    return imports

def get_writeable_import():
    """Returns a writeable import directory"""

    return import_list()[0]


#pylint: disable=too-many-public-methods
#pylint: disable=no-member


class CnssChecks(iof_ionss_verify.IonssVerify,
                 iof_ionss_setup.IonssExport):
    """A object purely to define test methods.

    These methods are invoked on a node where projections are being imported,
    so it can access the CNSS through the ctrl filesystem.

    There is no side-channel access to the exports however.

    Additionally, for multi-node these tests are executed in parallel so need
    to be carefull to use unique filenames and should endevour to clean up
    after themselves properly.

    This class imports from unittest to get access to the self.fail() method
    """

    logger = logging.getLogger("TestRunnerLogger")
    import_dir = None
    export_dir = None
    cnss_prefix = None
    test_local = None

    @staticmethod
    def get_unique(parent):
        """Return a unique identifer for this user of the projection

        Note that several methods within the same process will get the same
        unique identifier."""

        return tempfile.mkdtemp(dir=parent, prefix='%s_%d_' %
                                (gethostname().split('.')[0], os.getpid()))

    def test_chmod_file(self):
        """chmod a file"""

        self.logger.info("Creating chmod file  at %s", self.import_dir)
        filename = os.path.join(self.import_dir, 'chmod_file')
        init_mode = stat.S_IRUSR|stat.S_IWUSR
        fd = os.open(filename, os.O_RDWR|os.O_CREAT, init_mode)
        os.close(fd)
        fstat = os.stat(filename)

        self.logger.info("st_mode is 0%o", fstat.st_mode)

        actual_mode = stat.S_IMODE(fstat.st_mode)
        self.logger.info("0%o", actual_mode)

        if actual_mode != init_mode:
            self.fail("Mode is incorrect 0%o 0%o" % (actual_mode, init_mode))

        new_mode = stat.S_IRUSR
        os.chmod(filename, new_mode)
        fstat = os.stat(filename)
        self.logger.info("st_mode is 0%o", fstat.st_mode)

        actual_mode = stat.S_IMODE(fstat.st_mode)
        self.logger.info("0%o", actual_mode)

        if actual_mode != new_mode:
            self.fail("Mode is correct 0%o 0%o" % (actual_mode, new_mode))

    def test_fchmod(self):
        """Fchmod a file"""

        self.logger.info("Creating fchmod file  at %s", self.import_dir)
        filename = os.path.join(self.import_dir, 'fchmod_file')
        init_mode = stat.S_IRUSR|stat.S_IWUSR
        fd = os.open(filename, os.O_RDWR|os.O_CREAT, init_mode)

        fstat = os.fstat(fd)
        self.logger.info("st_mode is 0%o", fstat.st_mode)

        actual_mode = stat.S_IMODE(fstat.st_mode)
        self.logger.info("0%o", actual_mode)

        if actual_mode != init_mode:
            self.fail("Mode is incorrect 0%o 0%o" % (actual_mode, init_mode))

        new_mode = stat.S_IRUSR
        os.fchmod(fd, new_mode)
        fstat = os.fstat(fd)
        self.logger.info("st_mode is 0%o", fstat.st_mode)

        actual_mode = stat.S_IMODE(fstat.st_mode)
        self.logger.info("0%o", actual_mode)

        if actual_mode != new_mode:
            self.fail("Mode is incorrect 0%o 0%o" % (actual_mode, new_mode))
        os.close(fd)

    def test_file_copy(self):
        """Copy a file into a projecton

        Basic copy, using large I/O.  No permissions or metadata are used.
        """

        filename = os.path.join(self.import_dir, 'ls')

        shutil.copyfile('/bin/ls', filename)
        if self.test_local:
            self.verify_file_copy()

    def test_file_ftruncate(self):
        """Truncate a file"""

        filename = os.path.join(self.import_dir, 't_file')
        self.logger.info("test_file_ftruncate %s", filename)

        fd = open(filename, 'w')
        fd.truncate()

        if os.stat(filename).st_size != 0:
            self.fail("File truncate to 0 failed")

        fd.truncate(100)
        if os.stat(filename).st_size != 100:
            self.fail("File truncate to 100 failed")

        fd.close()

    def test_file_open_new(self):
        """Create a new file"""

        self.logger.info("Create a new file  at %s", self.import_dir)
        filename = os.path.join(self.import_dir, 'test_file2')

        fd = open(filename, 'w')
        fd.close()

        fstat = os.stat(filename)
        if not stat.S_ISREG(fstat.st_mode):
            self.fail("Failed to create a regular file")

    @unittest.skip("Test not complete")
    def test_file_open(self):
        """Open a file for reading

        This is supposed to fail, as the file doesn't exist.
        """

        filename = os.path.join(self.import_dir, 'non_exist_file')

        fd = open(filename, 'r')
        fd.close()

    def test_file_read_empty(self):
        """Read from a empty file"""

        filename = os.path.join(self.import_dir, 'empty_file')

        fd = open(filename, 'w')
        fd.close()

        fd = open(filename, 'r')

        if os.stat(filename).st_size != 0:
            self.fail("File is not empty.")

        fd.read()
        fd.close()

    def test_file_read_zero(self):
        """Read 0 bytes from a file"""

        tfile = os.path.join(self.import_dir, 'zero_file')

        fd = os.open(tfile, os.O_RDWR|os.O_CREAT)
        ret = os.read(fd, 10)
        if ret.decode():
            self.fail("Failed to return zero bytes"  %ret.decode())
        os.close(fd)

    def test_file_rename(self):
        """Write to a file"""

        filename = os.path.join(self.import_dir, 'c_file')

        fd = open(filename, 'w')
        fd.write('World')
        fd.close()

        new_file = os.path.join(self.import_dir, 'd_file')
        os.rename(filename, new_file)

    def test_file_sync(self):
        """Sync a file"""

        filename = os.path.join(self.import_dir, 'sync_file')

        fd = os.open(filename, os.O_RDWR|os.O_CREAT)
        os.write(fd, bytes("Hello world", 'UTF-8'))
        os.fsync(fd)

        os.lseek(fd, 0, 0)
        data = os.read(fd, 100).decode('UTF-8')

        if data != 'Hello world':
            self.fail('File contents wrong %s' % data)
        else:
            self.logger.info("Contents read from the synced file: %s", data)

        os.ftruncate(fd, 100)
        os.close(fd)

    def test_file_truncate(self):
        """Write to a file"""

        filename = os.path.join(self.import_dir, 'truncate_file')

        fd = open(filename, 'w')
        fd.write('World')
        fd.close()

        fd = open(filename, 'w')
        fd.write('World')
        fd.close()

    def test_file_unlink(self):
        """Create and remove a file"""

        filename = os.path.join(self.import_dir, 'unlink_file')

        fd = open(filename, 'w')
        fd.close()
        os.unlink(filename)

    def test_mkdir(self):
        """Create a directory and check it exists

        This test is also a bit of a nonsense as it makes a directory, however
        it relies on mkdtemp() is get_unique() in order to launch, so if there
        is a problem it'll be the setup which will fail, not the test.
        """

        ndir = os.path.join(self.import_dir, 'new_dir')

        self.logger.info(self.id())

        self.logger.info("Creating new directory at %s", ndir)

        os.mkdir(ndir)

        if not os.path.isdir(ndir):
            self.fail("Newly created directory does not exist")

        self.logger.info(os.listdir(ndir))

        os.rmdir(ndir)

    def test_mnt_path(self):
        """Check that mount points do not contain dot characters"""

        for mnt in import_list():
            self.assertFalse('.' in mnt, 'mount point should not contain .')

    def test_rmdir(self):
        """Remove a directory"""

        ndir = os.path.join(self.import_dir, 'my_dir')

        os.mkdir(ndir)

        self.logger.info("Directory contents:")
        self.logger.info(os.listdir(ndir))

        os.rmdir(ndir)

    def test_set_time(self):
        """Set the time of a file"""

        filename = os.path.join(self.import_dir, 'time_file')

        fd = open(filename, 'w')
        fd.close()

        stat_info = os.stat(filename)
        self.logger.info("Stat results before setting time:")
        self.logger.info(stat_info)
        time.sleep(2)
        os.utime(filename)
        stat_info = os.stat(filename)
        self.logger.info("Stat results after setting time (sleep for 2s):")
        self.logger.info(stat_info)

    # These methods have both multi node and iof_test_local component

    def test_file_copy_from(self):
        """Copy a file into a projection

        Basic copy, using large I/O.  No permissions or metadata are used.
        """

        if self.test_local:
            self.export_file_copy_from()

        filename = os.path.join(self.import_dir, 'ls')
        dst_file = os.path.join(self.export_dir, 'ls.2')
        self.logger.info("test_file_copy_from %s", filename)
        self.logger.info("test_file_copy_from to: %s", dst_file)

        shutil.copyfile(filename, dst_file)
        if self.test_local:
            self.verify_file_copy_from()

    def test_file_open_existing(self):
        """Open a existing file for reading"""

        if self.test_local:
            self.export_file_open_existing()

        filename = os.path.join(self.import_dir, 'exist_file')
        self.logger.info("test_file_open_existing %s", filename)
        fd = open(filename, 'r')
        fd.close()

    def test_file_read(self):
        """Read from a file"""

        if self.test_local:
            self.export_file_read()

        filename = os.path.join(self.import_dir, 'read_file')
        self.logger.info("test_file_read %s", filename)
        with open(filename, 'r') as fd:
            data = fd.read()

        if data != 'Hello':
            self.fail('File contents wrong %s %s' % ('Hello', data))
        else:
            self.logger.info("Contents from file read: %s %s", 'Hello', data)

    def test_file_write(self):
        """Write to a file"""

        filename = os.path.join(self.import_dir, 'write_file')
        with open(filename, 'w') as fd:
            fd.write('World')

        if self.test_local:
            self.verify_file_write()

    def test_ionss_link(self):
        """CORFSHIP-336 Check that stat does not deference symlinks"""
        # Make a directory 'b', create a symlink from 'a' to 'b'
        # and then stat() it to see what type it is

        if self.test_local:
            self.export_ionss_link()

        e_b = os.lstat(os.path.join(self.import_dir, 'a'))
        if self.test_local:
            self.verify_clean_up_ionss_link()

        self.logger.info("test_ionss_link %s", e_b)
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
        if self.test_local:
            self.export_ionss_self_listdir()

        dirs = os.listdir(self.import_dir)

        self.logger.info(dirs)
        if self.test_local:
            self.verify_clean_up_ionss_self_listdir()

    def test_make_symlink(self):
        """Make a symlink"""

        os.symlink('mlink_target',
                   os.path.join(self.import_dir, 'mlink_source'))
        self.logger.info(os.listdir(self.import_dir))

        if self.test_local:
            self.verify_make_symlink()

    def test_many_files(self):
        """Create lots of files, and then perform readdir"""

        if self.test_local:
            self.export_many_files()

        test_dir = os.path.join(self.import_dir, 'many')
        files = []
        for x in range(0, 100):
            this_file = 'file_%d' % x
            filename = os.path.join(test_dir, this_file)
            fd = open(filename, 'w')
            fd.close()
            files.append(this_file)

        import_list_files = os.listdir(test_dir)
        self.logger.info("test_many_files files %s", sorted(files))
        self.logger.info("test_many_files import_list_files %s",
                         sorted(import_list_files))
        # create a list of files to be checked in ionss verify
        file_list = os.path.join(self.import_dir, 'file_list')
        with open(file_list, 'w') as f:
            for item in sorted(files):
                f.write(item + '\n')

        if self.test_local:
            self.verify_many_files()

        if sorted(files) != sorted(import_list_files):
            self.fail("Import Directory contents are wrong")

    def test_read_symlink(self):
        """Read a symlink"""

        self.logger.info("List the files on CN")
        self.logger.info(os.listdir(self.import_dir))

        if self.test_local:
            self.export_read_symlink()

        rlink_source = os.path.join(self.import_dir, 'rlink_source')
        self.logger.info("stat rlink_source %s", os.lstat(rlink_source))
        result = os.readlink(rlink_source)
        self.logger.info("read rlink_source %s", result)

        if result != 'rlink_target':
            self.fail("Link target is wrong '%s'" % result)
        else:
            self.logger.info("Verified read on link target with source")

    def test_self_test(self):
        """Run self-test"""

        self_test = find_executable('self_test')
        if not self_test:
            cart_prefix = os.getenv("IOF_CART_PREFIX", None)
            if not cart_prefix:
                self.fail('Could not find self_test binary')
            self_test = os.path.join(cart_prefix, 'bin', 'self_test')
            if not os.path.exists(self_test):
                self.fail('Could not find self_test binary %s', self_test)
        environ = os.environ
        environ['CRT_PHY_ADDR_STR'] = self.crt_phy_addr
        environ['OFI_INTERFACE'] = self.ofi_interface
        cmd = [self_test, '--singleton', '--path', self.cnss_prefix,
               '--group-name', 'IONSS', '-e' '0:0',
               '-r', '1000', '-s' '0 0,0 128,128 0']

        log_path = os.path.join(os.getenv("IOF_TESTLOG", "output"), \
                                self.logdir_name())
        if not os.path.exists(log_path):
            os.makedirs(log_path)
        cmdfileout = os.path.join(log_path, "self_test.out")
        cmdfileerr = os.path.join(log_path, "self_test.err")
        procrtn = -1
        try:
            with open(cmdfileout, mode='w') as outfile, \
                open(cmdfileerr, mode='w') as errfile:
                outfile.write("{!s}\n  Command: {!s} \n{!s}\n".format(
                    ("=" * 40), (" ".join(cmd)), ("=" * 40)))
                outfile.flush()
                procrtn = subprocess.call(cmd, timeout=180, env=environ,
                                          stdout=outfile, stderr=errfile)
        except (FileNotFoundError) as e:
            self.logger.info("Testnss: %s", \
                             e.strerror)
        except (IOError) as e:
            self.logger.info("Testnss: Error opening the log files: %s", \
                             e.errno)
        if procrtn != 0:
            self.fail("cart self test failed: %s" % procrtn)

    @staticmethod
    def test_statfs():
        """Invoke statfs"""

        #for test_dir in common_methods.import_list():
        for test_dir in import_list():
            cmd = ['df', test_dir]
            os.system(' '.join(cmd))

    def test_use_ino(self):
        """Test that stat returns correct information"""

        filename = os.path.join(self.import_dir, 'test_ino_file')
        self.logger.info("test_use_ino %s", filename)

        fd = open(filename, 'w')
        fd.close()

        a = os.stat(filename)

        # Currently the FUSE plugin does not correctly report inodes
        # so currently there are differences

        cn_stats = {}
        for key in dir(a):
            if not key.startswith('st_'):
                continue
            elif key == 'st_dev':
                continue

            cn_stats[key] = str(Decimal(getattr(a, key)))

        stat_file = os.path.join(self.import_dir, 'cn_stat_output')
        with open(stat_file, 'w') as fd:
            json.dump(cn_stats, fd, indent=4, skipkeys=True)

        if self.test_local:
            self.verify_use_ino()
