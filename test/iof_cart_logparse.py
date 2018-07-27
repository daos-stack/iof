#!/usr/bin/env python3
# Copyright (C) 2018 Intel Corporation
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
IofLogIter class definition.

This provides a way of querying CaRT logfiles for processing.
"""

class ReadWithoutReset(Exception):
    """Exception to be raised when IofLogIter() is mis-used"""
    pass

class InvalidPid(Exception):
    """Exception to be raised when invalid pid is requested"""
    pass

# pylint: disable=too-few-public-methods
class IofLogIter():
    """Class for parsing CaRT log files

    This class implements a iterator for lines in a cart log file.  The iterator
    is rewindable, and there are options for automatically skipping lines.
    """

    def __init__(self, fname):
        """Load a file, and check how many processes have written to it"""
        self._fname = fname

        pids = []
        self._fd = open(self._fname, 'r')
        self._eof = False

        self._pid_str = None
        self._trace_only = False

        for line in self:
            fields = line.split()
            pid = int(fields[2][5:-1])
            if pid not in pids:
                pids.append(pid)

        self._pids = sorted(pids)

    def __del__(self):
        self._fd.close()

    def __iter__(self):
        return self

    def __next__(self):
        if self._eof:
            raise ReadWithoutReset

        while True:
            line = self._fd.readline()
            if line == '':
                self._eof = True
                raise StopIteration

            if len(line) == 1:
                continue

            fields = line.split()
            if len(fields[0]) != 17:
                continue
            if len(fields) < 6:
                continue

            if self._pid_str and fields[2] != self._pid_str:
                continue

            if self._trace_only and fields[6] != 'TRACE:':
                continue

            return line[:-1]

    def get_pids(self):
        """Return an array of pids appearing in the file"""
        return self._pids

    def reset(self, pid=None, trace_only=False):
        """Rewind file iterator, and set options

        If pid is set the the iterator will only return lines matchine the pid
        If trace_only is True then the iterator will only return trace lines.
        """
        self._fd.seek(0)
        self._eof = False

        if pid is not None:
            if pid not in self._pids:
                raise InvalidPid
            self._pid_str = "CaRT[%d]" % pid
        else:
            self._pid_str = None
        self._trace_only = trace_only
