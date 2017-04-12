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

#pylint: disable=too-many-public-methods

import os
import sys
import time
import stat
import shutil
import getpass
import subprocess
import tempfile
import logging
import unittest
import iofcommontestsuite
import common_methods


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

        if self.logger.getEffectiveLevel() == logging.WARNING:
            self.logger.setLevel(logging.INFO)
            __ch = logging.StreamHandler(sys.stdout)
            self.logger.addHandler(__ch)

        self.logger.info("Starting for %s", self.id())

        # Allow the use of a custom temp directory.  This can be needed on
        # docker when /tmp is an overlay fs.
        export_tmp_dir = os.getenv("IOF_TMP_DIR", '/tmp')

        self.import_dir = tempfile.mkdtemp(prefix='tmp_iof_test_import_',
                                           dir=export_tmp_dir)
        self.base_dir = os.path.join(self.import_dir, 'exp')
        common_methods.CTRL_DIR = os.path.join(self.import_dir, '.ctrl')
        self.shutdown_file = os.path.join(common_methods.CTRL_DIR, 'shutdown')
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

        log_mask = os.getenv("CRT_LOG_MASK", "INFO")

        valgrind = iofcommontestsuite.valgrind_suffix(log_path)

        cmd = [orterun,
               '--output-filename', log_path]

        if getpass.getuser() == 'root':
            cmd.append('--allow-run-as-root')
        cmd.extend(['-n', '1',
                    '-x', 'CRT_LOG_MASK=%s' % log_mask,
                    '-x', 'CNSS_PREFIX=%s' % self.import_dir])
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
            if not self.check_process(self.proc):
                procrtn = self.common_stop_process(self.proc)
                self.fail("orterun process did not start %d" % procrtn)
            time.sleep(1)

        self.logger.info("Running")

    def tearDown(self):
        """tear down the test"""

        stats_dir = os.path.join(self.import_dir, '.ctrl', 'iof',
                                 'projections', '0', 'stats')
        if not os.path.exists(stats_dir):
            self.fail("Stats dir missing")

        self.logger.info("Dumping statistics for filesystem 0")
        for stat_file in os.listdir(stats_dir):
            f = open(os.path.join(stats_dir, stat_file), 'r')
            data = f.read()
            f.close()
            self.logger.info("%s:%s", stat_file, data.rstrip("\n"))

        # Firstly try and shutdown the filesystems cleanly
        if self.is_running():
            f = open(self.shutdown_file, 'w')
            f.write('1')
            try:
                f.close()
            except OSError:
                pass

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
            mp = os.path.join(self.import_dir, mount)
            try:
                self.common_launch_cmd("", "fusermount -q -u %s" % mp)
            except FileNotFoundError:
                pass
            os.rmdir(mp)

        # Finally, remove any temporary files created.
        os.rmdir(self.import_dir)
        shutil.rmtree(self.export_dir)
        os.rmdir(self.e_dir)

        # Now exit if there was an error after the rest of the cleanup has
        # completed.
        if procrtn == 42:
            self.fail("Job completed with valgrind errors")
        elif procrtn != 0:
            self.fail("Non-zero exit code from orterun %d" % procrtn)

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

    def test_use_ino(self):
        """Test that stat returns correct information"""

        filename = os.path.join(self.import_dir, 'exp', 'test_file')

        fd = open(filename, 'w')
        fd.close()

        a = os.stat(filename)
        b = os.stat(os.path.join(self.export_dir, 'test_file'))

        self.logger.info(a)
        self.logger.info(b)

        # Currently the FUSE plugin does not correctly report inodes
        # so currently there are differences

        if a != b:
            self.logger.info("File stat data is different")

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

        if len(diffs):
            self.fail("Stat attributes are different %s" % diffs)

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
        import_list = os.listdir(test_dir)

        self.logger.info(sorted(files))
        self.logger.info(sorted(export_list))
        self.logger.info(sorted(import_list))

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

    def test_file_write(self):
        """Write to a file"""

        filename = os.path.join(self.import_dir, 'exp', 'write_file')

        fd = open(filename, 'w')
        fd.write('World')
        fd.close()

        tfile = os.path.join(self.export_dir, 'write_file')

        fd = open(tfile, 'r')
        data = fd.read()
        fd.close()

        if data != 'World':
            self.fail('File contents wrong %s %s' % ('Hello', data))

    def test_file_copy_from(self):
        """Copy a file into a projecton

        Basic copy, using large I/O.  No permissions or metadata are used.
        """

        src_file = os.path.join(self.export_dir, 'ls')
        shutil.copyfile('/bin/ls', src_file)

        filename = os.path.join(self.import_dir, 'exp', 'ls')
        dst_file = os.path.join(self.export_dir, 'ls.2')

        shutil.copyfile(filename, dst_file)

    def test_read_symlink(self):
        """Read a symlink"""

        os.symlink('target', os.path.join(self.export_dir, 'source'))

        self.logger.info(os.listdir(os.path.join(self.import_dir, 'exp')))

        self.logger.info(os.lstat(os.path.join(self.import_dir,
                                               'exp',
                                               'source')))

        result = os.readlink(os.path.join(self.import_dir, 'exp', 'source'))
        if result != 'target':
            self.fail("Link target is wrong '%s'" % result)

    def test_make_symlink(self):
        """Make a symlink"""

        os.symlink('target', os.path.join(self.import_dir, 'exp', 'source'))

        self.logger.info(os.listdir(os.path.join(self.import_dir, 'exp')))

        self.logger.info(os.lstat(os.path.join(self.export_dir, 'source')))
        result = os.readlink(os.path.join(self.export_dir, 'source'))
        if result != 'target':
            self.fail("Link target is wrong '%s'" % result)

    def test_ioil(self):
        """Run the interception library test"""

        dirname = os.path.dirname(os.path.realpath(__file__))
        test_path = os.path.join(dirname, '..', 'tests')

        if not os.path.exists(test_path):
            test_path = os.path.join(dirname, '..', 'install', os.uname()[0],
                                     'TESTING', 'tests')

        for tname in ['s_test_ioil', 'lf_s_test_ioil']:
            testname = os.path.join(test_path, tname)
            if not os.path.exists(testname):
                self.skipTest("%s executable not found" % tname)

            self.logger.info("libioil test - input string:\n %s\n", testname)
            procrtn = subprocess.call([testname, self.import_dir], timeout=180)
            if procrtn != 0:
                self.fail("IO interception test failed: %s" % procrtn)

    @unittest.skip("Fails on FUSE2")
    def test_file_read_rename(self):
        """Read from a file which has been renamed on the backend"""

        # Create a file on the export location.
        tfile = os.path.join(self.export_dir, 'a_file')

        fd = open(tfile, 'w')
        fd.write("Hello")
        fd.close()

        # Open it through the projection
        filename = os.path.join(self.import_dir, 'exp', 'a_file')
        fd = open(filename, 'r')

        # Rename it on the backend, so any FUSE cache is out of sync
        os.rename(tfile, os.path.join(self.export_dir, 'b_file'))

        # Now read and check the data.
        data = fd.read()
        fd.close()

        if data != 'Hello':
            self.fail('File contents wrong %s %s' % ('Hello', data))

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
        for possible in dir(self):
            if not possible.startswith('test_'):
                continue

            subtest_count += 1
            obj = getattr(self, possible)
            print('Running test %s' % possible)
            with self.subTest(possible[5:]):
                obj()
                idir = os.path.join(self.export_dir)
                files = os.listdir(idir)
                for e in files:
                    ep = os.path.join(idir, e)
                    print("Cleaning up %s" % ep)
                    if os.path.isfile(ep) or os.path.islink(ep):
                        os.unlink(ep)
                    elif os.path.isdir(ep):
                        shutil.rmtree(ep)
                files = os.listdir(idir)
                if len(files):
                    self.fail('Test left some files %s' % files)

        print("Ran %d subtests" % (subtest_count))

def local_launch():
    """Launch a local filesystem for interactive use"""

    fs = Testlocal()
    fs.setUp()

    print("IOF projections up and running")
    print(fs.import_dir)
    print("Ctrl-C to shutdown or run")
    print("echo 1 > %s/.ctrl/shutdown" % fs.import_dir)
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

    uargs = [sys.argv[0]]
    iargs = list(sys.argv)
    iargs.pop(0)

    tests_to_run = []
    launch = False
    if len(iargs):

        tests = []
        for a_test in dir(Testlocal):
            if not a_test.startswith('test_'):
                continue

            tests.append(a_test[5:])

        unknown = False
        for arg in iargs:
            if arg == '--valgrind':
                os.environ['TR_USE_VALGRIND'] = 'memcheck-native'
            elif arg.startswith('--log-mask='):
                os.environ['CRT_LOG_MASK'] = arg[11:]
            elif arg == '--redirect':
                os.environ['TR_REDIRECT_OUTPUT'] = 'yes'
            elif arg in ['-h', '--help']:
                print("""
In addition to the options below the following options are supported

  --valgrind        Run the test under valgrind
  --redirect        Redirect daemon output to file
  --log-mask=<MASK> Set the CaRT log mask to MASK
  --launch          Launch a local filesystem for interactive use

Test methods can be specified directly on the command line""")
                print("Valid test names are %s" % ','.join(sorted(tests)))
                print("")
                uargs.append(arg)
            elif arg == '--launch':
                launch = True
            elif arg in tests:
                tests_to_run.append('Testlocal.test_%s' % arg)
            else:
                unknown = True
                uargs.append(arg)

        if unknown:
            print("Unknown option given, passing through to unittest")
            print("Valid test names are %s" % ','.join(sorted(tests)))

    if launch:
        rc = local_launch()
        sys.exit(rc)

    if len(tests_to_run) == 0:
        tests_to_run.append('Testlocal.go')

    unittest.main(defaultTest=tests_to_run, argv=uargs)
