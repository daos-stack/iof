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

python3 -m unittest -c iof_test_local

Or to launch a specific test by running:

python3 -m unittest -c iof_test_local.Testlocal.test_ionss_link

"""

import os
import sys
import stat
import time
import shutil
import getpass
import subprocess
import tempfile
import logging
import unittest
#pylint: disable=import-error
#pylint: disable=no-name-in-module
from distutils.spawn import find_executable
#pylint: enable=import-error
#pylint: enable=no-name-in-module
import iofcommontestsuite
import common_methods

sys.path.append('install/Linux/TESTING/scripts')
try:
    #Python/C Shim
    #pylint: disable=import-error
    import iofmod
    have_iofmod = True
    #pylint: enable=import-error
except ImportError:
    have_iofmod = False

#pylint: disable=too-many-public-methods
#pylint: disable=too-many-statements
#pylint: disable=too-many-instance-attributes


class Testlocal(unittest.TestCase,
                common_methods.CnssChecks,
                iofcommontestsuite.CommonTestSuite):
    """Local test"""

    proc = None
    cnss_prefix = None
    import_dir = None
    export_dir = None
    shutdown_file = None
    active_file = None
    e_dir = None
    log_mask = ""
    crt_phy_addr = ""
    ofi_interface = ""
    test_local = True

    def is_running(self):
        """Check if the cnss is running"""

        data = "0"

        try:
            fd = open(self.active_file, 'r')
            data = fd.read()
            fd.close()
        except FileNotFoundError:
            return False

        if data.rstrip() == '1':
            return True
        return False

    def logdir_name(self):
        """create the log directory name"""

        # Append the test case to the log directory to get unique names.
        # Do this in a way that matches the dump_error_messages() logic
        # in the test runner so that on failure only failing methods are
        # shown.

        parts = self.id().split('.')
        method = parts[2]
        if method.startswith('test_'):
            method = method[5:]
        return os.path.join(parts[1], method)

    def setUp(self):
        """set up the test"""

        if self.logger.getEffectiveLevel() == logging.WARNING:
            self.logger.setLevel(logging.INFO)
            __ch = logging.StreamHandler(sys.stdout)
            self.logger.addHandler(__ch)

        self.logger.info("Starting for %s", self.id())

        # set the standalone test flag
        self.test_local = True
        # Allow the use of a custom temp directory.  This can be needed on
        # docker when /tmp is an overlay fs.
        export_tmp_dir = os.getenv("IOF_TMP_DIR", '/tmp')

        self.cnss_prefix = tempfile.mkdtemp(prefix='tmp_iof_test_import_',
                                            dir=export_tmp_dir)
        self.import_dir = os.path.join(self.cnss_prefix, 'exp')
        common_methods.CTRL_DIR = os.path.join(self.cnss_prefix, '.ctrl')
        self.shutdown_file = os.path.join(common_methods.CTRL_DIR, 'shutdown')
        self.active_file = os.path.join(common_methods.CTRL_DIR, 'active')
        self.e_dir = tempfile.mkdtemp(prefix='tmp_iof_test_export_',
                                      dir=export_tmp_dir)

        ompi_bin = os.getenv('IOF_OMPI_BIN', None)

        if ompi_bin:
            orterun = os.path.realpath(os.path.join(ompi_bin, 'orterun'))
        else:
            orterun = 'orterun'

        self.export_dir = os.path.join(self.e_dir, 'exp')
        os.mkdir(self.export_dir)

        log_path = os.getenv("IOF_TESTLOG",
                             os.path.join(os.path.dirname(
                                 os.path.realpath(__file__)), 'output'))

        # Append the test case to the log directory to get unique names.
        # Do this in a way that matches the dump_error_messages() logic
        # in the test runner so that on failure only failing methods are
        # shown.

        log_path = os.path.join(log_path, self.logdir_name())

        valgrind = iofcommontestsuite.valgrind_suffix(log_path)

        default_log_mask = "INFO,CTRL=WARN"
        if valgrind:
            default_log_mask = "DEBUG,MEM=WARN,CTRL=WARN"
        self.log_mask = os.getenv("D_LOG_MASK", default_log_mask)
        self.crt_phy_addr = os.getenv("CRT_PHY_ADDR_STR", "ofi+sockets")
        self.ofi_interface = os.getenv("OFI_INTERFACE", "lo")

        self.test_valgrind = iofcommontestsuite.valgrind_suffix(log_path,
                                                                pmix=False)
        ionss_args = ['ionss']

        cmd = [orterun,
               '--output-filename', log_path]

        if getpass.getuser() == 'root':
            cmd.append('--allow-run-as-root')
        cmd.extend(['-n', '1',
                    '-x', 'D_LOG_MASK=%s' % self.log_mask,
                    '-x', 'CRT_PHY_ADDR_STR=%s' % self.crt_phy_addr,
                    '-x', 'OFI_INTERFACE=%s' % self.ofi_interface,
                    '-x', 'CNSS_PREFIX=%s' % self.cnss_prefix])
        cmd.extend(valgrind)
        cmd.extend(['cnss',
                    ':',
                    '-n', '3',
                    '-x', 'CRT_PHY_ADDR_STR=%s' % self.crt_phy_addr,
                    '-x', 'OFI_INTERFACE=%s' % self.ofi_interface,
                    '-x', 'D_LOG_MASK=%s' % self.log_mask])
        cmd.extend(valgrind)
        cmd.extend(ionss_args)
        cmd.extend([self.export_dir, '/usr'])

        # cmd.append('--poll-interval=1')

        self.proc = self.common_launch_process('', ' '.join(cmd))

        if not valgrind:
            waittime = 30
        else:
            waittime = 120
        elapsed_time = 0
        while not self.is_running():
            elapsed_time += 1
            if elapsed_time > waittime:
                self.fail("Could not detect startup in %s seconds" % waittime)
            if not self.check_process(self.proc):
                procrtn = self.common_stop_process(self.proc)
                self.fail("orterun process did not start %d" % procrtn)
            time.sleep(1)

        self.logger.info("Running")

    def mark_log(self, msg):
        """Log a message to stdout and to the CNSS logs"""

        log_file = os.path.join(self.cnss_prefix, '.ctrl', 'write_log')
        print(msg)
        with open(log_file, 'w')  as fd:
            fd.write(msg)

    def show_stats(self):
        """Display projection statistics to stdout"""

        proj_dir = os.path.join(self.cnss_prefix, '.ctrl', 'iof',
                                'projections')

        for idx in os.listdir(proj_dir):

            stats_dir = os.path.join(proj_dir, idx, 'stats')

            if not os.path.exists(stats_dir):
                self.fail("Stats dir missing")

            mount_point = None

            with open(os.path.join(proj_dir, idx, 'mount_point'), 'r') as f:
                mount_point = f.read().strip()

            self.logger.info('Dumping statistics for filesystem %s',
                             mount_point)
            for stat_file in sorted(os.listdir(stats_dir)):
                with open(os.path.join(stats_dir, stat_file), 'r') as f:
                    data = f.read()
                    f.close()
                    value = data.rstrip()
                    if value != '0':
                        self.logger.info("%s:%s", stat_file, value)

    def tearDown(self):
        """tear down the test"""

        self.show_stats()

        # Firstly try and shutdown the filesystems cleanly
        if self.is_running():
            f = open(self.shutdown_file, 'w')
            f.write('1')
            f.close()

        procrtn = 0
        # Now try to kill the orterun process
        if self.proc is not None:
            procrtn = self.common_stop_process(self.proc)

        self.cleanup(procrtn)

    def cleanup(self, procrtn):
        """Delete any temporary files or directories created"""

        # Call fusermount on mount points in case there are stale mounts.
        # Remove the mount directories
        for mount in ['.ctrl', 'usr', 'exp']:
            mp = os.path.join(self.cnss_prefix, mount)
            try:
                self.common_launch_cmd("", "fusermount -q -u %s" % mp)
            except FileNotFoundError:
                pass
            os.rmdir(mp)

        # Finally, remove any temporary files created.
        os.unlink("%s/IONSS.attach_info_tmp" % self.cnss_prefix)
        os.rmdir(self.cnss_prefix)
        shutil.rmtree(self.export_dir)
        os.rmdir(self.e_dir)

        # Now exit if there was an error after the rest of the cleanup has
        # completed.
        if procrtn == 42:
            self.fail("Job completed with valgrind errors")
        elif procrtn != 0:
            self.fail("Non-zero exit code from orterun %d" % procrtn)

    def test_ro_stat(self):
        """Test that stat works on read-only projections"""

        filename = os.path.join(self.cnss_prefix, 'usr', 'bin')

        b = os.stat(os.path.join('/usr', 'bin'))
        a = os.stat(filename)

        self.logger.info(a)
        self.logger.info(b)

        diffs = []

        for key in dir(a):
            if not key.startswith('st_'):
                continue
            av = getattr(a, key)
            bv = getattr(b, key)
            self.logger.info("Key %s import %s export %s", key, av, bv)
            if key == 'st_dev':
                continue
            if av != bv:
                self.logger.error("Keys are differnet")
                diffs.append(key)

        if diffs:
            self.fail("Stat attributes are different %s" % diffs)

    def test_ro_link(self):
        """Test that stat works on read-only projections"""

        # Find the smallest file in the range of 1MB to 8MB.
        target_file = None
        u_stat = os.stat('/usr')
        bin_files = sorted(os.listdir('/usr/bin'))
        for bfile in bin_files:
            fname = os.path.join('/usr/bin/', bfile)
            s = os.lstat(fname)
            if s.st_dev != u_stat.st_dev:
                self.skipTest("Inconsistent device for /usr files")
            if not stat.S_ISLNK(s.st_mode):
                continue
            target_file = bfile
            break

        if target_file is None:
            self.fail("Could not find file in /usr/bin")

        s_target = os.readlink(os.path.join('/usr/bin', target_file))

        self.logger.info("File is %s", target_file)
        self.logger.info("Target is %s", s_target)

        test_file = os.path.join(self.cnss_prefix, 'usr', 'bin', target_file)
        ro_target = os.readlink(test_file)

        self.logger.info("Read-only target is %s", s_target)

        if ro_target != s_target:
            self.fail("Link target is wrong, %s %s" % (s_target, ro_target))

    def test_direct_read(self):
        """Read a large file"""

        # Find the smallest file in the range of 1MB to 8MB.
        target_file = None
        target_file_size = 1024 * 1024 * 8
        u_stat = os.stat('/usr')
        bin_files = sorted(os.listdir('/usr/bin'))
        for bfile in bin_files:
            fname = os.path.join('/usr/bin/', bfile)
            s = os.lstat(fname)
            if s.st_dev != u_stat.st_dev:
                self.skipTest("Inconsistent device for /usr files")
            if not stat.S_ISREG(s.st_mode):
                continue
            if not os.access(fname, os.R_OK):
                continue
            if s.st_size < 1024 * 1024:
                continue
            if s.st_size < target_file_size:
                target_file = bfile
                target_file_size = s.st_size

        if target_file is None:
            self.fail("Could not find file in /usr/bin")

        self.logger.info('Test file is %s %d', target_file, target_file_size)
        test_file = os.path.join(self.cnss_prefix, 'usr', 'bin', target_file)

        cmd = 'dd if=%s of=/dev/null bs=64k iflag=direct' % test_file
        rtn = self.common_launch_cmd('dd', cmd)
        if rtn != 0:
            self.fail('DD returned error')

    def test_direct_write(self):
        """Write a large file"""
        test_file = os.path.join(self.import_dir, 'test_file')

        cmd = 'dd if=/dev/zero of=%s bs=4k count=8 oflag=direct' % test_file
        rtn = self.common_launch_cmd('dd', cmd)
        if rtn != 0:
            self.fail('DD returned error')

    def test_file_basic_write(self):
        """Write to a existing file"""

        filename = os.path.join(self.export_dir, 'write_file')
        with open(filename, 'w') as fd:
            fd.close()

        filename = os.path.join(self.import_dir, 'write_file')
        with open(filename, 'w') as fd:
            fd.write('World')
            fd.close()

    def test_ro_listdir(self):
        """Read directory contents"""

        test_dir = os.path.join(self.cnss_prefix, 'usr', 'bin')
        files = os.listdir(test_dir)

        if not files:
            self.fail("No files in /usr/bin")

    def test_large_read(self):
        """Read a large file"""

        # Find the smallest file in the range of 1MB to 8MB.
        target_file = None
        target_file_size = 1024 * 1024 * 8
        u_stat = os.stat('/usr')
        bin_files = sorted(os.listdir('/usr/bin'))
        for bfile in bin_files:
            fname = os.path.join('/usr/bin/', bfile)
            s = os.lstat(fname)
            if s.st_dev != u_stat.st_dev:
                self.skipTest("Inconsistent device for /usr files")
            if not stat.S_ISREG(s.st_mode):
                continue
            if not os.access(fname, os.R_OK):
                continue
            if s.st_size < 1024 * 1024:
                continue
            if s.st_size < target_file_size:
                target_file = bfile
                target_file_size = s.st_size

        if target_file is None:
            self.fail("Could not find file in /usr/bin")

        self.logger.info('Test file is %s %d', target_file, target_file_size)
        test_file = os.path.join(self.cnss_prefix, 'usr', 'bin', target_file)

        cmd = 'dd if=%s of=/dev/null bs=4k' % test_file
        rtn = self.common_launch_cmd('dd', cmd)
        if rtn != 0:
            self.fail('DD returned error')

        cmd = 'dd if=%s of=/dev/null bs=65k' % test_file
        rtn = self.common_launch_cmd('dd', cmd)
        if rtn != 0:
            self.fail('DD bs=65k returned error')

    def test_file_read_rename(self):
        """Read from a file which has been renamed on the backend"""

        # Create a file on the export location.
        tfile = os.path.join(self.export_dir, 'a_file')

        fd = open(tfile, 'w')
        fd.write("Hello")
        fd.close()

        # Open it through the projection
        filename = os.path.join(self.import_dir, 'a_file')
        fd = open(filename, 'r')

        # Rename it on the backend, so any FUSE cache is out of sync
        os.rename(tfile, os.path.join(self.export_dir, 'b_file'))

        # Now read and check the data.
        data = fd.read()
        fd.close()

        if data != 'Hello':
            self.fail('File contents wrong %s %s' % ('Hello', data))

    def test_ioil(self):
        """Run the interception library test"""
        # Check the value of il_ioctl before execution
        stats_dir = os.path.join(self.cnss_prefix, '.ctrl', 'iof',
                                 'projections', '0', 'stats')

        if not os.path.exists(stats_dir):
            self.fail("Stats dir missing.")

        if not os.path.isfile(os.path.join(stats_dir, 'il_ioctl')):
            self.fail("il_ioctl missing in stats.")
        else:
            stat_file = os.path.join(stats_dir, 'il_ioctl')
            f = open(stat_file, 'r')
            data = f.read()
            f.close()
            initial_il_ioctl = data.rstrip("\n")
            self.logger.info("initial il_ioctl: %s", initial_il_ioctl)

        dirname = os.path.dirname(os.path.realpath(__file__))
        test_path = os.path.join(dirname, '..', 'tests')

        if not os.path.exists(test_path):
            test_path = os.path.join(dirname, '..', 'install', os.uname()[0],
                                     'TESTING', 'tests')

        environ = os.environ
        environ['CNSS_PREFIX'] = self.cnss_prefix
        environ['D_LOG_MASK'] = self.log_mask
        environ['CRT_PHY_ADDR_STR'] = self.crt_phy_addr
        environ['OFI_INTERFACE'] = self.ofi_interface
        for tname in ['s_test_ioil', 'lf_s_test_ioil']:
            testname = os.path.join(test_path, tname)
            if not os.path.exists(testname):
                self.skipTest("%s executable not found" % tname)

            self.logger.info("libioil test - input string:\n %s\n", testname)
            # set this to match value used by this job
            cmd = []
            cmd.extend(self.test_valgrind)
            cmd.extend([testname])
            procrtn = subprocess.call(cmd, timeout=180,
                                      env=environ)

            if procrtn != 0:
                self.fail("IO interception test failed: %s" % procrtn)

        # Check the value of il_ioctl after execution
        f = open(stat_file, 'r')
        data = f.read()
        f.close()
        final_il_ioctl = data.rstrip("\n")
        self.logger.info("Final il_ioctl: %s", final_il_ioctl)
        # Compare il_ioctl value after the test run with initial value or
        # if il_ioctl value is less than 12 (expected value is atleast 12)
        if final_il_ioctl <= initial_il_ioctl or int(final_il_ioctl) < 12:
            self.fail("IO interception library failed to attach," + \
                " il_ioctl: %s" % final_il_ioctl)

    def test_mdtest(self):
        """Run mdtest"""
        topdir = self.import_dir

        mdtest_cmdstr = "/testbin/mdtest/bin/mdtest"
        if not os.path.exists(mdtest_cmdstr):
            mdtest_cmdstr = "mdtest"
        mdtest_cmdstr = find_executable(mdtest_cmdstr)
        if not mdtest_cmdstr:
            self.skipTest('mdtest not installed')
        mdtest = [mdtest_cmdstr, '-d', topdir]
        short_run = list(mdtest)
        short_run.extend(['-i', '3', '-I', '10'])
        start_time = time.time()
        rtn = self.common_launch_cmd('mdtest', ' '.join(short_run))
        elapsed = time.time() - start_time
        print('Mdtest returned %d in %d seconds' % (rtn, elapsed))
        if rtn != 0:
            self.fail("Mdtest test_failed, rc = %d" % rtn)
        if elapsed > 5 or self.test_valgrind:
            return
        long_run = list(mdtest)
        long_run.extend(['-i', '5', '-I', '1000'])
        start_time = time.time()
        rtn = self.common_launch_cmd(mdtest_cmdstr, ' '.join(long_run))
        elapsed = time.time() - start_time
        print('Mdtest returned %d in %d seconds' % (rtn, elapsed))
        if rtn != 0:
            self.fail("Mdtest test_failed, rc = %d" % rtn)

    @unittest.skipUnless(have_iofmod, "needs iofmod")
    def test_iofmod(self):
        """Calls all C tests present in C/Python shim"""

        if not have_iofmod:
            self.skipTest("iofmod not loadable")

        subtest_count = 0
        for possible in sorted(dir(iofmod)):
            if not possible.startswith('test_'):
                continue

            obj = getattr(iofmod, possible)
            if not callable(obj):
                continue

            subtest_count += 1
            with self.subTest(possible):
                self.mark_log('Starting test iofmod.%s' % possible)
                fd = iofmod.open_test_file(self.import_dir)
                if fd is None:
                    self.fail('File descriptor returned null on open')

                ret = obj(fd)
                self.logger.info("%s returned %s", possible, ret)
                if ret is None:
                    self.fail('%s returned null' % possible)

                ret = iofmod.close_test_file(fd)
                if ret is None:
                    self.fail('File not closed successfully')
                self.mark_log('Finished test iofmod.%s, cleaning up' % possible)

                self.clean_export_dir()

        return subtest_count

    def clean_export_dir(self):
        """Clean up files created in backend fs"""
        idir = os.path.join(self.export_dir)
        files = os.listdir(idir)
        for e in files:
            ep = os.path.join(idir, e)
            print('Cleaning up %s' % ep)
            if os.path.isfile(ep) or os.path.islink(ep):
                os.unlink(ep)
            elif os.path.isdir(ep):
                shutil.rmtree(ep)
        files = os.listdir(idir)
        if files:
            self.fail('Test left some files %s' % files)

    def go(self):
        """A wrapper method to invoke all methods as subTests"""

        # This method invokes all other test_* methods as subtests which has
        # the effect that they're all run on the same projection.  This is both
        # much faster but also means that it's more difficult to isolate the
        # tests.

        # After each test remove any left-over files from the export directory
        # to avoid using IOF for this.  Use the export rather than the import
        # point here so that problems in IOF are not reported against the test,
        # for example if there is a problem with unlink we do not want the
        # write tests to fail.
        # It also means that tests can interact with each other, for example
        # any kernel caching of entries or negative entries can cause issues
        # with tests not seeing the files they expect.  To avoid that use
        # unique files for each test.  What we should do is either setup a new
        # subdirectory under import/export for each test, or disable cacheing
        # in the kernel.
        subtest_count = 0
        for possible in sorted(dir(self)):
            if not possible.startswith('test_'):
                continue

            # Skip the iofmod methods as these are called directly later.
            if 'iofmod' in possible:
                continue

            obj = getattr(self, possible)
            if not callable(obj):
                continue

            subtest_count += 1
            with self.subTest(possible[5:]):
                self.mark_log('Starting test %s' % possible)
                obj()
                self.mark_log('Finished test %s, cleaning up' % possible)
                self.clean_export_dir()

        if have_iofmod:
            subtest_count += self.test_iofmod()

        print("Ran %d subtests" % (subtest_count))

def local_launch(ioil_dir):
    """Launch a local filesystem for interactive use"""

    fs = Testlocal()
    fs.setUp()

    print("IOF projections up and running")
    print(fs.cnss_prefix)
    print("Ctrl-C to shutdown or run")
    print("echo 1 > %s/.ctrl/shutdown" % fs.cnss_prefix)
    print("")
    print("To use the interception library, set ")
    print("LD_PRELOAD=%s/libioil.so or " % ioil_dir)
    print("link that library in your application before system libraries.  ")
    print("The latter option makes debugging simpler.")
    print("")
    my_rc = 0
    try:
        my_rc = fs.wait_process(fs.proc)
    except KeyboardInterrupt:
        print("Caught Ctrl-C")
        my_rc = 1
    fs.cleanup(my_rc)

    return my_rc

if __name__ == '__main__':
    import argparse

    # Invoke testing if this command is run directly
    #
    # Add command-line passing here so that the user can specify individual
    # tests to run easily, and to enable other configuration optins.
    #
    # An example command might be the following:
    #
    # ./iof_test_local.py --valgrind --log-mask=DEBUG --redirect chmod
    #
    # Unrecognised args are passed through to unittest.main().
    cnss_path = find_executable('cnss')
    if not cnss_path:
        print("cnss executable not found.  Run setup script first")
        sys.exit(1)
    iof_root = os.path.dirname(os.path.dirname(cnss_path))
    ioil_path = os.path.join(iof_root, 'lib')

    tests = []
    for a_test in dir(Testlocal):
        if not a_test.startswith('test_'):
            continue

        tests.append(a_test[5:])

    test_help = "A test to run (%s) or a utest argument" % ', '.join(tests)
    parser = argparse.ArgumentParser(description='Run local iof tests')
    parser.add_argument('tests', metavar='TEST', type=str,
                        help=test_help, nargs='*')
    parser.add_argument('--valgrind', action='store_true',
                        help='Run the test under valgrind')
    parser.add_argument('--redirect', action='store_true',
                        help='Redirect daemon output to a file')
    parser.add_argument('--launch', action='store_true',
                        help='Launch a local file system for interactive use')
    parser.add_argument('--log-mask', dest='mask', metavar='MASK', type=str,
                        default='INFO,CTRL=WARN', help='Set the CaRT log mask')
    args = parser.parse_args()

    if args.valgrind:
        os.environ['TR_USE_VALGRIND'] = 'memcheck-native'
    if args.redirect:
        os.environ['TR_REDIRECT_OUTPUT'] = 'yes'
    os.environ['D_LOG_MASK'] = args.mask

    tests_to_run = []
    uargs = []

    for test in args.tests:
        if test in tests:
            tests_to_run.append('Testlocal.test_%s' % test)
        else:
            uargs.append(test)

    if uargs:
        print("Unknown option given, passing through to unittest")
        print("Valid test names are %s" % ','.join(sorted(tests)))

    uargs.insert(0, sys.argv[0])

    if args.launch:
        rc = local_launch(ioil_path)
        sys.exit(rc)

    if not tests_to_run:
        tests_to_run.append('Testlocal.go')

    unittest.main(defaultTest=tests_to_run, argv=uargs)
