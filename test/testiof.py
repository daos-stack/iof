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

""" Helper code for iof_test_ionss.py to run on CN """

import os
import sys
import time

class TestIof():
    """IOF filesystem tests in private access mode"""

    def __init__(self, cnss_prefix):
        self.start_dir = cnss_prefix
        self.ctrl_dir = os.path.join(self.start_dir, ".ctrl")

    def iof_fs_test(self):
        """Test private access mount points"""
        print("starting to stat the mountpoints")
        entry = os.path.join(self.ctrl_dir, "iof", "PA")
        if os.path.isdir(entry):
            for mntfile in os.listdir(entry):
                myfile = os.path.join(entry, mntfile)
                fd = open(myfile, "r")
                mnt_path = fd.readline().strip()
                print("Mount path is %s" % mnt_path)
                stat_obj = os.stat(mnt_path)

                print(stat_obj)
            return True

        else:
            print("Mount points not found")
            return False

    def iofstarted(self):
        """Wait for ctrl fs to start"""
        if not os.path.isdir(self.start_dir):
            print("prefix is not a directory %s" % self.start_dir)
            return False
        filename = os.path.join(self.ctrl_dir, 'shutdown')
        i = 30
        while i > 0:
            i = i - 1
            time.sleep(1)
            if os.path.exists(filename) is True:
                print(os.stat(filename))
                return True
        print("Unable to detect filesystem")
        return False

    def iofshutdown(self):
        """Shutdown iof"""
        filename = os.path.join(self.ctrl_dir, 'shutdown')
        with open(filename, 'a'):
            os.utime(filename, None)

        return True

if __name__ == "__main__":


    print("Test IOF starting")
    assert len(sys.argv) > 1

    prefix = sys.argv[1]
    print("CNSS Prefix: %s " % prefix)
    mytest = TestIof(prefix)
    assert mytest.iofstarted()
    assert mytest.iof_fs_test()
    assert mytest.iofshutdown()
