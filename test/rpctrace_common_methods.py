#!/usr/bin/env python3
# Copyright (C) 2017-2018 Intel Corporation
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
RpcTrace class definition
Methods associated to RPC tracing for error debuging.
"""

import os
from collections import OrderedDict
import common_methods
import iof_cart_logparse

# CaRT Error numbers to convert to strings.
C_ERRNOS = {0: '-DER_SUCCESS',
            -1011: '-DER_TIMEDOUT',
            -1032: '-DER_EVICTED'}

# pylint: disable=too-many-locals
# pylint: disable=too-many-statements
# pylint: disable=too-many-branches
# pylint: disable=too-many-instance-attributes
class RpcTrace(common_methods.ColorizedOutput):
    """RPC tracing methods
    RPC = CaRT Priv
    Descriptor = handle/iof_pol_type/iof_pool/iof_projection_info/iof_state/
    cnss_plugin/plugin_entry/cnss_info"""

    #dictionary maps descriptor to descriptor type and parent
    desc_dict = {}

    ALLOC_STATE = 'ALLOCATED'
    DEALLOC_STATE = 'DEALLOCATED'
    SUBMIT_STATE = 'SUBMITTED'
    SENT_STATE = 'SENT'
    COMPLETED_STATE = 'COMPLETED'

    # A list of allowed descriptor state transitions, the key is the new
    # state, the value is a list of previous states which are expected.
    STATE_CHANGE_TABLE = {ALLOC_STATE: (DEALLOC_STATE),
                          DEALLOC_STATE: (ALLOC_STATE,
                                          SUBMIT_STATE,
                                          SENT_STATE,
                                          COMPLETED_STATE),
                          SUBMIT_STATE: (ALLOC_STATE),
                          SENT_STATE: (SUBMIT_STATE,
                                       COMPLETED_STATE),
                          COMPLETED_STATE: (SENT_STATE,
                                            SUBMIT_STATE,
                                            ALLOC_STATE)}

    VERBOSE_STATE_TRANSITIONS = False
    VERBOSE_LOG = True

    def __init__(self, fname, output_stream):
        self.set_log(output_stream)
        self.input_file = fname

        #index_multiprocess will determine which PID to trace
        #(ie index 0 will be assigned the first PID found in logs)

        self.lf = iof_cart_logparse.IofLogIter(self.input_file)
        self.desc_table = None
        self._to_trace = None
        self._to_trace_fuse = None

    def rpc_reporting(self, index_multiprocess=None):
        """RPC reporting for RPC state machine"""

        pids = self.lf.get_pids()

        if index_multiprocess is not None:
            #index_multiprocess is None if there is only one process being
            #traced, otherwise first index should start at 0
            pid_to_trace = pids[index_multiprocess]
            self._rpc_reporting_pid(pid_to_trace)
            return

        if len(pids) != 1:
            self.error_output("Multiprocess log file")
            return

        self._rpc_reporting_pid(pids[0])

    def _rpc_error_state_tracing(self, rpc_dict, rpc, rpc_state):
        """Error checking for rpc state"""

        # Returns a tuple of (State, Extra string)
        status = None
        message = None
        if rpc in rpc_dict:
            if rpc_dict[rpc] in self.STATE_CHANGE_TABLE[rpc_state]:
                status = 'SUCCESS'
            else:
                status = 'ERROR'
                message = 'previous state: {}'.format(rpc_dict[rpc])

        elif rpc_state == self.ALLOC_STATE:
            status = 'SUCCESS'
        else:
            status = 'WARN'
            message = "no alloc'd state registered"

        if rpc_state == self.DEALLOC_STATE:
            del rpc_dict[rpc]
        else:
            rpc_dict[rpc] = rpc_state
        return (status, message)

    def _rpc_reporting_pid(self, pid_to_trace):
        """RPC reporting for RPC state machine, for mutiprocesses"""
        rpc_dict = {}
        op_state_counters = {}
        c_states = {}
        c_state_names = set()

        # Use to convert from descriptor to opcode.
        current_opcodes = {}

        self.normal_output('CaRT RPC Reporting:\nLogfile: {}, '
                           'PID: {}\n'.format(self.input_file,
                                              pid_to_trace))

        output_table = []

        for line in self.lf.new_iter(pid=pid_to_trace):
            rpc_state = None
            opcode = None
            rpc = None

            if line.endswith('allocated.') or \
               line.endswith('allocated per RPC request received.'):
                rpc_state = self.ALLOC_STATE
                opcode = line.get_field(5)[:-2]
            elif line.endswith('), decref to 0.'):
                rpc_state = self.DEALLOC_STATE
            elif line.endswith('submitted.'):
                rpc_state = self.SUBMIT_STATE
            elif line.endswith(' sent.'):
                rpc_state = self.SENT_STATE
            elif line.is_callback():
                rpc = line.get_field(6)
                rpc_state = self.COMPLETED_STATE
                result = line.get_field(-1).rstrip('.')
                result = C_ERRNOS.get(int(result), result)
                c_state_names.add(result)
                opcode = current_opcodes[rpc]
                try:
                    c_states[opcode][result] += 1
                except KeyError:
                    try:
                        c_states[opcode][result] = 1
                    except KeyError:
                        c_states[opcode] = {}
                        c_states[opcode][result] = 1
            else:
                continue

            if not rpc:
                rpc = line.get_field(3)

            if rpc_state == self.ALLOC_STATE:
                current_opcodes[rpc] = opcode
            else:
                opcode = current_opcodes[rpc]
            if rpc_state == self.DEALLOC_STATE:
                del current_opcodes[rpc]

            if opcode not in op_state_counters:
                op_state_counters[opcode] = {self.ALLOC_STATE :0,
                                             self.DEALLOC_STATE: 0,
                                             self.SENT_STATE:0,
                                             self.COMPLETED_STATE:0,
                                             self.SUBMIT_STATE:0}
            op_state_counters[opcode][rpc_state] += 1

            (state, extra) = self._rpc_error_state_tracing(rpc_dict,
                                                           rpc,
                                                           rpc_state)

            if self.VERBOSE_STATE_TRANSITIONS or state != 'SUCCESS':
                output_table.append([state,
                                     rpc,
                                     rpc_state,
                                     opcode,
                                     line.function,
                                     extra])

        if output_table:
            self.table_output(output_table,
                              title='RPC State Transitions:',
                              headers=['STATE',
                                       'RPC',
                                       'STATE',
                                       'Op',
                                       'Function',
                                       'Extra'])

        table = []
        errors = []
        names = sorted(c_state_names)
        if names:
            try:
                names.remove('-DER_SUCCESS')
            except ValueError:
                pass
            names.insert(0, '-DER_SUCCESS')
        headers = ['OPCODE',
                   self.ALLOC_STATE,
                   self.SUBMIT_STATE,
                   self.SENT_STATE,
                   self.COMPLETED_STATE,
                   self.DEALLOC_STATE]

        for state in names:
            headers.append(state)
        for (op, counts) in sorted(op_state_counters.items()):
            row = [op,
                   counts[self.ALLOC_STATE],
                   counts[self.SUBMIT_STATE],
                   counts[self.SENT_STATE],
                   counts[self.COMPLETED_STATE],
                   counts[self.DEALLOC_STATE]]
            for state in names:
                try:
                    row.append(c_states[op].get(state, ''))
                except KeyError:
                    row.append('')
            table.append(row)
            if counts[self.ALLOC_STATE] != counts[self.DEALLOC_STATE]:
                errors.append("ERROR: Opcode {}: Alloc'd Total = {}, "
                              "Dealloc'd Total = {}". \
                              format(op,
                                     counts[self.ALLOC_STATE],
                                     counts[self.DEALLOC_STATE]))
            # Sent can be more than completed because of corpcs but shouldn't
            # be less
            if counts[self.SENT_STATE] > counts[self.COMPLETED_STATE]:
                errors.append("ERROR: Opcode {}: sent Total = {}, "
                              "Completed Total = {}". \
                              format(op,
                                     counts[self.SENT_STATE],
                                     counts[self.COMPLETED_STATE]))

        self.table_output(table,
                          title='Opcode State Transition Tally',
                          headers=headers,
                          stralign='right')

        if errors:
            self.list_output(errors)

    #************ Descriptor Tracing Methods (IOF_TRACE macros) **********

    def _descriptor_error_state_tracing(self):
        """Check for any descriptors that are not registered/deregistered"""

        desc_state = OrderedDict()
        # Maintain a list of parent->children relationships.  This is a dict of
        # sets, using the parent as the key and the value is a set of children.
        # When a descriptor is deleted then all child RPCs in the set are also
        # removed from desc_state
        linked = {}

        output_table = []

        for line in self.lf.new_iter(trace_only=True):
            state = None
            desc = line.descriptor
            if desc == '':
                continue

            msg = None
            is_error = True

            if line.is_new():
                state = 'Registered'
                if desc in desc_state:
                    msg = 'previous state: {}'.format(desc_state[desc])
                else:
                    is_error = False
                desc_state[desc] = state
                linked[desc] = set()
            if line.is_link():
                state = 'Linked'
                if desc in desc_state:
                    msg = 'Bad link'
                else:
                    is_error = False
                desc_state[desc] = state
                parent = line.parent
                if parent in linked:
                    linked[parent].add(desc)
            elif line.is_dereg():
                state = 'Deregistered'
                if desc not in desc_state:
                    msg = 'Not registered'
                elif desc_state.get(desc, None) == 'Registered':
                    del desc_state[desc]
                    is_error = False
                else:
                    msg = desc_state[desc]
                if desc in linked:
                    for child in linked[desc]:
                        del desc_state[child]
                    del linked[desc]
            else:
                continue

            if is_error:
                self.have_errors = True

            if not self.VERBOSE_STATE_TRANSITIONS and not is_error:
                continue

            if is_error:
                output_table.append([desc,
                                     '{} (Error)'.format(state),
                                     line.function,
                                     msg,
                                     '{}: {}'.format(line.index,
                                                     line.get_msg())])
            else:
                output_table.append([desc, state, line.function, None, None])

        if output_table:
            self.table_output(output_table,
                              title='Descriptor State Transitions:',
                              headers=['Descriptor',
                                       'State',
                                       'Function',
                                       'Message',
                                       'Line'])

        #check if all descriptors are deregistered
        for d, state in desc_state.items():
            if state == 'Registered':
                self.error_output('{} is not Deregistered'.format(d))
            else:
                self.error_output('{}:{} not Deregistered from state'.\
                                  format(d, state))

    def descriptor_rpc_trace(self):
        """Parses twice thru log to create a hierarchy of descriptors and also
           a dict storing all RPCs tied to a descriptor"""

        self.desc_table = OrderedDict()
        self.desc_dict = {}
        self.normal_output('IOF Descriptor/RPC Tracing:\n'
                           'Logfile: {}'.format(self.input_file))

        reuse_table = {}
        self._descriptor_error_state_tracing()

        fuse_file = os.path.join('src', 'ioc', 'ops')
        to_trace = None
        to_trace_fuse = None

        for line in self.lf.new_iter(trace_only=True):

            if not to_trace and \
               (line.level <= iof_cart_logparse.LOG_LEVELS['WARN']):
                reuse_count = reuse_table.get(line.descriptor, 0)
                # Is this correct?  Need a logfile which tests this.
                if reuse_count > 0:
                    assert False
                    to_trace = "{}_{}".format(line.descriptor, reuse_count)
                else:
                    to_trace = line.descriptor

            if line.is_new():

                if not to_trace and \
                   not to_trace_fuse and fuse_file in line.filename:
                    to_trace_fuse = line.descriptor

                #register a new descriptor/object in the log hierarchy
                new_obj = line.descriptor
                parent = line.parent

                if parent in reuse_table and reuse_table[parent] > 0:
                    parent = '{}_{}'.format(parent, reuse_table[parent])
                obj_type = line.get_field(-3)[1:-1]

                if new_obj in reuse_table:
                    reuse_table[new_obj] += 1
                    new_obj = '{}_{}'.format(new_obj, reuse_table[new_obj])
                else:
                    reuse_table[new_obj] = 0

                self.desc_table[new_obj] = (obj_type, parent)
                self.desc_dict[new_obj] = []

            if not line.is_link():
                continue

            # register RPCs tied to given handle
            # Link lines are a little odd in the log file in that they are
            # the wrong way round, line.parent is the descriptor and
            # line.descriptor is the new RPC.

            desc = line.parent

            if desc in reuse_table and reuse_table[desc] > 0:
                desc = '{}_{}'.format(desc, reuse_table[desc])
            if desc in self.desc_dict:
                self.desc_dict[desc].append((line.descriptor,
                                             line.get_field(-3)[1:-1]))
            else:
                self.error_output('Descriptor {} is not present'.format(desc))

        self._to_trace = to_trace
        self._to_trace_fuse = to_trace_fuse

    def _rpc_trace_output_hierarchy(self, descriptor):
        """Prints full TRACE hierarchy for a given descriptor"""
        output = []
        #append all descriptors/rpcs in hierarchy for log dump
        traces_for_log_dump = []

        trace = descriptor
        #append an extra line
        output.append('')
        try:
            while trace != "root":
                traces_for_log_dump.append(trace)
                (trace_type, parent) = self.desc_table[trace]
                output.append('{}: {}'.format(trace_type, trace))
                for (desc, name) in self.desc_dict[trace]:
                    traces_for_log_dump.append(desc)
                    output.append('\t{} {}'.format(name, desc))
                trace = parent
        except KeyError:
            self.error_output('Descriptor {} does not trace back to root'\
                              .format(descriptor))

        traces_for_log_dump.append(trace)

        if not output:
            self.error_output('Descriptor {} not currently registered or '
                              'linked'.format(descriptor))
        else:
            output.insert(0, '\nDescriptor Hierarchy ({}):'.format(descriptor))
            self.list_output(output)

        return traces_for_log_dump

    def _rpc_trace_output_logdump(self, descriptor):
        """Prints all log messages relating to the given descriptor or any
           pointer in the descriptor's hierarchy"""

        traces_for_log_dump = self._rpc_trace_output_hierarchy(descriptor)
        descriptors = []
        if traces_for_log_dump:
            #the descriptor for which to start tracing
            trace = traces_for_log_dump[0]
        else:
            self.error_output('Descriptor is null for logdump')
        position_desc_cnt = 0
        #location is used for log location of descriptor if it is reused
        #for ex "0x12345_1" would indicate location 1 (2nd instance in logs)
        try:
            location = int(trace.split('_')[1])
        except IndexError:
            location = None

        #remove all "_#" endings on list of descriptors to correctly match
        for d in traces_for_log_dump:
            descriptors.append(d.split('_')[0])
        desc = descriptors[0] #the first descriptor w/o suffix

        self.log_output('\nLog dump for descriptor hierarchy ({}):'\
                        .format(trace))

        # Set after something interesting happens, and means the remainder
        # of the file should just be logged.
        drain_file = False

        for line in self.lf.new_iter(raw=True):

            if drain_file:
                if line.trace and line.descriptor in descriptors:
                    self.log_output(line.to_str(mark=True))
                elif self.VERBOSE_LOG:
                    self.log_output(line.to_str())
                continue

            desc_log_marked = False
            if line.is_new():
                if line.descriptor != desc:
                    #print the remaining non-relevant log messages
                    if self.VERBOSE_LOG:
                        self.log_output(line.to_str())
                    continue
                if location is None:#tracing a unique descriptor
                    self.log_output(line.to_str(mark=True))
                    drain_file = True
                    continue
                #start the log dump where the specific reused descriptor
                #is registered, logging will include all instances of
                #this descriptor after
                if position_desc_cnt == location:
                    self.log_output(line.to_str(mark=True))
                    drain_file = True
                    continue
                position_desc_cnt += 1

            if not desc_log_marked and self.VERBOSE_LOG:
                #print the remaining non-relevant log messages
                self.log_output(line.to_str())

    def rpc_trace_output(self, descriptor):
        """Dumps all RPCs tied to a descriptor, descriptor hierarchy, and all
           log messages related to descriptor"""
        missing_links = []
        output_table = []

        for key in self.desc_table:
            (stype, par) = self.desc_table[key]

            rpcs = []
            if key in self.desc_dict:
                for i in self.desc_dict[key]:
                    rpcs.append('{} {}'.format(i[0], i[1]))

            if par in self.desc_table:
                (par_type, _) = self.desc_table[par]
                parent_string = "{} {}".format(par, par_type)
            elif par == 'root':
                parent_string = par

            else: #fail if missing parent/link
                parent_string = "{} [None]".format(par)
                missing_links.append(par)

            output_table.append([key,
                                 stype,
                                 '\n'.join(rpcs),
                                 parent_string])

        self.normal_output('')
        self.table_output(output_table,
                          headers=['Descriptor',
                                   'Type',
                                   'RPCs',
                                   'Parent'])

        #Log dump for descriptor hierarchy
        self._rpc_trace_output_logdump(descriptor)

        return missing_links

    def descriptor_to_trace(self):
        """Find the file handle to use for descriptor tracing:
        if an error or warning is found in trace logs, use that descriptor,
        otherwise use first fuse op instance in logs"""

        if self._to_trace:
            self.normal_output('Tracing descriptor {} with error/warning'. \
                               format(self._to_trace))
            return self._to_trace

        if self._to_trace_fuse:
            return self._to_trace_fuse

        self.error_output('Descriptor not found to trace')
        return None
