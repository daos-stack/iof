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
IofLogLine class definition.

This provides a way of querying CaRT logfiles for processing.
"""

class InvalidPid(Exception):
    """Exception to be raised when invalid pid is requested"""
    pass

class InvalidLogFile(Exception):
    """Exception to be raised when log file cannot be parsed"""
    pass

LOG_LEVELS = {'FATAL' :1,
              'CRIT'  :2,
              'ERR'   :3,
              'WARN'  :4,
              'NOTE'  :5,
              'INFO'  :6,
              'DBUG'  :7}

# pylint: disable=too-few-public-methods
class IofLogRaw():
    """Class for raw (non cart log lines) in cart log files

    This is used for lines that cannot be identified as cart log lines,
    for example mercury logs being sent to the same file.
    """
    def __init__(self, line):
        self.line = line.rstrip('\n')
        self.trace = False

    def to_str(self):
        """Convert the object to a string, in a way that is compatible with
        IofLogLine
        """
        return self.line
# pylint: enable=too-few-public-methods

# pylint: disable=too-many-instance-attributes
class IofLogLine():
    """Class for parsing CaRT log lines

    This class implements a way of inspecting individual lines of a log
    file.

    It allows for queries such as 'string in line' which will match against
    the message only, and != which will match the entire line.

    index is the line in the file, starting at 1.
    """
    def __init__(self, line, index):
        fields = line.split()
        # Work out the end of the fixed-width portion, and the beginning of the
        # message.  The hostname and pid fields are both variable width
        idx = 29 + len(fields[1]) + len(fields[2])
        self.pid = int(fields[2][5:-1])
        self._preamble = line[:idx]
        self.index = index
        try:
            self.level = LOG_LEVELS[fields[4]]
        except KeyError:
            raise InvalidLogFile(fields[4])

        self._fields = fields[5:]
        # Handle old-style trace messages.
        if self._fields[1] == 'TRACE:':
            self.trace = True
            self._fields.pop(1)
        elif self._fields[1][-2:] == '()':
            self.trace = False
            self.function = self._fields[1][:-2]
        elif self._fields[1][-1:] == ')':
            self.trace = True
        else:
            self.trace = False

        if self.trace:
            fn_str = self._fields[1]
            start_idx = fn_str.find('(')
            self.function = fn_str[:start_idx]
            desc = fn_str[start_idx+1:-1]
            if desc == '(nil)':
                self.descriptor = ''
            else:
                self.descriptor = desc
        self._msg = ' '.join(self._fields)

    def to_str(self, mark=False):
        """Convert the object to a string"""
        if mark:
            return '{} ** {}'.format(self._preamble, self._msg)
        return '{}    {}'.format(self._preamble, self._msg)

    def __contains__(self, item):
        idx = self._msg.find(item)
        if idx != -1:
            return True
        return False

    def endswith(self, item):
        """Mimic the str.endswith() function

        This only matches on the actual string part of the message, not the
        timestamp/pid/faculty parts.
        """
        return self._msg.endswith(item)

    def get_field(self, idx):
        """Return a specific field from the line"""
        return self._fields[idx]
# pylint: enable=too-many-instance-attributes

class IofLogIter():
    """Class for parsing CaRT log files

    This class implements a iterator for lines in a cart log file.  The iterator
    is rewindable, and there are options for automatically skipping lines.
    """

    def __init__(self, fname):
        """Load a file, and check how many processes have written to it"""

        fd = open(fname, 'r')
        self._data = []
        index = 0
        pids = set()
        for line in fd:
            fields = line.split(maxsplit=8)
            index += 1
            if len(fields) < 6 or len(fields[0]) != 17:
                self._data.append(IofLogRaw(line))
            else:
                l_obj = IofLogLine(line, index)
                pids.add(l_obj.pid)
                self._data.append(l_obj)
        fd.close()

        # Offset into the file when iterating.  This is an array index, and is
        # based from zero, as opposed to line index which is based from 1.
        self._offset = 0

        self._pid = None
        self._trace_only = False
        self._raw = False
        self._pids = sorted(pids)

    def __iter__(self):
        return self

    def __next__(self):

        while True:
            try:
                line = self._data[self._offset]
            except IndexError:
                raise StopIteration
            self._offset += 1

            if not self._raw and isinstance(line, IofLogRaw):
                continue

            if self._trace_only and not line.trace:
                continue

            if self._pid and line.pid != self._pid:
                continue

            return line

    def get_pids(self):
        """Return an array of pids appearing in the file"""
        return self._pids

# pylint: disable=too-many-arguments
    def reset(self,
              pid=None,
              trace_only=False,
              raw=False,
              index=1):
        """Rewind file iterator, and set options

        If pid is set the the iterator will only return lines matchine the pid
        If trace_only is True then the iterator will only return trace lines.
        if raw is set then all lines in the file are returned, even non-log
        lines.
        Index is the line number in the file to start from.
        """
        self._offset = index - 1

        if pid is not None:
            if pid not in self._pids:
                raise InvalidPid
            self._pid = pid
        else:
            self._pid = None
        self._trace_only = trace_only
        self._raw = raw
# pylint: enable=too-many-arguments
