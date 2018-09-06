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

# pylint: disable=too-many-locals
# pylint: disable=too-many-statements
# pylint: disable=too-many-branches
# pylint: disable=too-many-instance-attributes
# pylint: disable=too-many-nested-blocks
class RpcTrace(common_methods.ColorizedOutput):
    """RPC tracing methods
    RPC = CaRT Priv
    Descriptor = handle/iof_pol_type/iof_pool/iof_projection_info/iof_state/
    cnss_plugin/plugin_entry/cnss_info"""

    rpc_dict = {}
    """dictionary maps rpc to opcode"""
    trace_dict = {}
    """dictionary maps descriptor to descriptor type and parent"""
    desc_dict = {}
    """dictionary maps descriptor to 'registered/deregistered/linked' states
    w/ regard to IOF_TRACE macros"""

    ALLOC_STATE = 'ALLOCATED'
    DEALLOC_STATE = 'DEALLOCATED'
    SUBMIT_STATE = 'SUBMITTED'
    SENT_STATE = 'SENT'
    STATES = [ALLOC_STATE, SUBMIT_STATE, SENT_STATE, DEALLOC_STATE]
    VERBOSE_STATE_TRANSITIONS = False
    VERBOSE_LOG = True

    def __init__(self, fname, output_stream):
        self.set_log(output_stream)
        self.input_file = fname

        #index_multiprocess will determine which PID to trace
        #(ie index 0 will be assigned the first PID found in logs)

        self.lf = iof_cart_logparse.IofLogIter(self.input_file)
        self.desc_table = None

    def _rpc_error_state_tracing(self, rpc, rpc_state):
        """Error checking for rpc state"""

        # Returns a tuple of (State, Extra string)
        status = None
        message = None
        if rpc in self.rpc_dict:
            if (rpc_state == self.ALLOC_STATE) and \
               (self.rpc_dict[rpc] == self.DEALLOC_STATE):
                status = 'SUCCESS'
            elif (rpc_state == self.DEALLOC_STATE) and \
                 (self.rpc_dict[rpc] != self.DEALLOC_STATE):
                status = 'SUCCESS'
            elif (rpc_state == self.SUBMIT_STATE) and \
                 (self.rpc_dict[rpc] == self.ALLOC_STATE):
                status = 'SUCCESS'
            elif (rpc_state == self.SENT_STATE) and \
                 (self.rpc_dict[rpc] == self.SUBMIT_STATE):
                status = 'SUCCESS'
            else:
                status = 'ERROR'
                message = 'previous state: {}'.format(self.rpc_dict[rpc])
        else:
            if rpc_state == self.ALLOC_STATE:
                status = 'SUCCESS'
            else:
                status = 'WARN'
                message = "no alloc'd state registered"


        if rpc_state == self.DEALLOC_STATE:
            del self.rpc_dict[rpc]
        else:
            self.rpc_dict[rpc] = rpc_state
        return (status, message)

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

    def _rpc_reporting_pid(self, pid_to_trace):
        """RPC reporting for RPC state machine, for mutiprocesses"""
        self.rpc_dict = {}
        op_state_counters = {}

        # Use to convert from descriptor to opcode.
        current_opcodes = {}

        self.normal_output('CaRT RPC Reporting:\nLogfile: {}, '
                           'PID: {}\n'.format(self.input_file,
                                              pid_to_trace))

        output_table = []

        self.lf.reset(pid=pid_to_trace)
        for line in self.lf:
            rpc_state = None
            opcode = None

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
            else:
                continue

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
                                             self.SUBMIT_STATE:0}
            op_state_counters[opcode][rpc_state] += 1

            (state, extra) = self._rpc_error_state_tracing(rpc, rpc_state)

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
        for (op, counts) in sorted(op_state_counters.items()):
            table.append([op,
                          counts[self.ALLOC_STATE],
                          counts[self.SUBMIT_STATE],
                          counts[self.SENT_STATE],
                          counts[self.DEALLOC_STATE]])
            if counts[self.ALLOC_STATE] != counts[self.DEALLOC_STATE]:
                errors.append("ERROR: Opcode {}: Alloc'd Total = {}, "
                              "Dealloc'd Total = {}". \
                              format(op,
                                     counts[self.ALLOC_STATE],
                                     counts[self.DEALLOC_STATE]))

        self.table_output(table,
                          title='Opcode State Transition Tally',
                          headers=['OPCODE'] + self.STATES)

        if errors:
            self.list_output(errors)

    #************ Descriptor Tracing Methods (IOF_TRACE macros) **********

    def descriptor_rpc_trace(self):
        """Parses twice thru log to create a hierarchy of descriptors and also
           a dict storing all RPCs tied to a descriptor"""

        # desc_table and trace_dict contain the same information, however the
        # way it is presented is different.  The code should be moving away
        # from trace_dict where possible
        self.desc_table = OrderedDict()
        self.trace_dict = {}
        self.desc_dict = {}
        self.normal_output('IOF Descriptor/RPC Tracing:\n'
                           'Logfile: {}'.format(self.input_file))

        reuse_table = {}
        self._descriptor_error_state_tracing()

        self.lf.reset(trace_only=True)
        for line in self.lf:
            if "Registered new" in line:

                #register a new descriptor/object in the log hierarchy
                new_obj = line.descriptor
                parent = line.get_field(-1)

                if parent in reuse_table and reuse_table[parent] > 0:
                    parent = '{}_{}'.format(parent, reuse_table[parent])
                obj_type = line.get_field(-3)[1:-1]

                if new_obj in reuse_table:
                    reuse_table[new_obj] += 1
                    new_obj = '{}_{}'.format(new_obj, reuse_table[new_obj])
                else:
                    reuse_table[new_obj] = 0

                self.trace_dict.setdefault(new_obj, []).append((obj_type,
                                                                parent))
                self.desc_table[new_obj] = (obj_type, parent)
                self.desc_dict[new_obj] = []

            if "Link" not in line:
                continue
            #register RPCs tied to given handle

            rpc = line.get_field(-1)
            rpc_type = line.get_field(-3)[1:-1]
            #find index of descriptor in order to append rpc
            #to correct index in the chance it is reused in the dict
            if rpc in reuse_table and reuse_table[rpc] > 0:
                index = '{}_{}'.format(rpc, reuse_table[rpc])
            else:
                index = rpc
            if index in self.desc_dict:
                self.desc_dict[index].append((line.descriptor, rpc_type))
            else:
                self.error_output('Descriptor {} is not present'.format(index))

    def _desc_is_rpc(self, descriptor):
        rpc_type_list = [v for (k, v) in self.desc_dict.items()]
        desc_is_rpc = bool(descriptor not in self.trace_dict and \
                           [item for sublist in rpc_type_list \
                            for item in sublist if item[0] == descriptor])
        return desc_is_rpc

    def _rpc_trace_output_hierarchy(self, descriptor):
        """Prints full TRACE hierarchy for a given descriptor"""
        output = []
        #append all descriptors/rpcs in hierarchy for log dump
        traces_for_log_dump = []
        #checking if descriptor is an rpc, in which case it is only linked
        #to other descriptors with TRACE
        desc_is_rpc = self._desc_is_rpc(descriptor)

        if desc_is_rpc: #tracing an rpc
            output.append('No hierarchy available - descriptor is an RPC')
            traces_for_log_dump.append(descriptor)
        else:
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

        #checking if descriptor is an rpc, in which case it is only linked
        #to other descriptors with TRACE

        desc_is_rpc = self._desc_is_rpc(descriptor)

        self.log_output('\nLog dump for descriptor hierarchy ({}):'\
                        .format(trace))

        self.lf.reset(raw=True)
        for line in self.lf:
            desc_log_marked = False
            if desc_is_rpc: #tracing an rpc
                if line.trace:
                    if line.descriptor == desc:
                        self.log_output(line.to_str(mark=True))
                        desc_log_marked = True
            elif line.trace and 'Registered new' in line:
                if line.descriptor != desc:
                    #print the remaining non-relevant log messages
                    self.log_output(line.to_str())
                    continue
                if location is None:#tracing a unique descriptor
                    self.log_output(line.to_str(mark=True))
                    for nxtline in self.lf:
                        #start log dump after "registered" log
                        desc_log_marked = False
                        if nxtline.trace:
                            if nxtline.descriptor in descriptors:
                                self.log_output(nxtline.to_str(mark=True))
                                desc_log_marked = True
                        if not desc_log_marked and self.VERBOSE_LOG:
                            #print the remaining non-relevant log messages
                            self.log_output(nxtline.to_str())
                    return
                #start the log dump where the specific reused descriptor
                #is registered, logging will include all instances of
                #this descriptor after
                if position_desc_cnt == location:
                    self.log_output(line.to_str(mark=True))
                    for nxtline in self.lf:
                        desc_log_marked = False
                        if nxtline.trace:
                            if nxtline.descriptor in descriptors:
                                self.log_output(nxtline.to_str(mark=True))
                                desc_log_marked = True
                        if not desc_log_marked and self.VERBOSE_LOG:
                            #print the remaining non-relevant log mesgs
                            self.log_output(nxtline.to_str())
                    return
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

    def _descriptor_error_state_tracing(self):
        """Check for any descriptors that are not registered/deregistered"""

        desc_state = OrderedDict()
        # Maintain a list of parent->children relationships.  This is a dict of
        # sets, using the parent as the key and the value is a set of children.
        # When a descriptor is deleted then all child RPCs in the set are also
        # removed from desc_state
        linked = {}

        output_table = []

        self.lf.reset(trace_only=True)
        for line in self.lf:
            state = None
            desc = line.descriptor
            if desc == '':
                continue

            msg = None
            is_error = True

            if 'Registered new' in line:
                state = 'Registered'
                if desc in desc_state:
                    msg = 'previous state: {}'.format(desc_state[desc])
                else:
                    is_error = False
                desc_state[desc] = state
                linked[desc] = set()
            elif 'Link' in line:
                state = 'Linked'
                if desc not in desc_state or \
                   desc_state[desc] == 'Linked':
                    is_error = False
                desc_state[desc] = state
                parent = line.get_field(-1)
                linked[parent].add(desc)
            elif 'Deregistered' in line:
                state = 'Deregistered'
                if desc_state.get(desc, None) == 'Registered':
                    del desc_state[desc]
                    is_error = False
                elif desc_state.get(desc, None) == 'Linked':
                    msg = 'Linked RPC'
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
                                     msg])
            else:
                output_table.append([desc, state, line.function, msg])

        if output_table:
            self.table_output(output_table,
                              title='Descriptor State Transitions:',
                              headers=['Descriptor',
                                       'State',
                                       'Function',
                                       'Message'])

        #check if all descriptors are deregistered
        for d, state in desc_state.items():
            if state == 'Registered':
                self.error_output('{} is not Deregistered'.format(d))
            else:
                self.error_output('{}:{} not Deregistered from state'.\
                                  format(d, state))

    def descriptor_to_trace(self):
        """Find the file handle to use for descriptor tracing:
        if an error or warning is found in trace logs, use that descriptor,
        otherwise use first fuse op instance in logs"""
        descriptor = None
        position_desc_cnt = 0
        fuse_file = os.path.join('src', 'ioc', 'ops')

        fuse_desc = None

        self.lf.reset(trace_only=True)
        for line in self.lf:

            # Make a note of the first fuse descriptor seen.
            if not fuse_desc and fuse_file in line and 'Registered new' in line:
                fuse_desc = line.descriptor

            # Skip over any log lines less important than Warnings.
            if line.level > iof_cart_logparse.LOG_LEVELS['WARN']:
                continue

            descriptor = line.descriptor
            self.warning_output('Tracing descriptor {} with error/warning'. \
                                format(descriptor))
            reused_descs = [v for k, v in self.trace_dict.items() \
                            if descriptor in k]
            if len(reused_descs) > 1:
                for nxtline in self.lf:
                    if 'Registered new' in nxtline:
                        if nxtline.descriptor == descriptor:
                            position_desc_cnt += 1

            pos = len(reused_descs) - position_desc_cnt - 1
            if pos > 0:
                descriptor = '{}_{}'.\
                             format(descriptor,
                                    len(reused_descs) - \
                                    position_desc_cnt - 1)
            return descriptor

        if not fuse_desc:
            self.error_output('Descriptor not found to trace')
        return fuse_desc
