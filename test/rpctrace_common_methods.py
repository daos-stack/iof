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

import string
import os
from collections import Counter
import tabulate
import common_methods

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
    """dictionary maps rpc to 'alloc/submit/sent/dealloc' state"""
    op_state_list = []
    """list entries (opcode, rpc state)"""
    rpc_op_dict = {}
    """dictionary maps rpc to opcode"""
    trace_dict = {}
    """dictionary maps descriptor to descriptor type and parent"""
    desc_dict = {}
    """dictionary maps descriptor to RPC(s)"""
    desc_state = {}
    """dictionary maps descriptor to 'registered/deregistered/linked' states
    w/ regard to IOF_TRACE macros"""

    ALLOC_STATE = 'ALLOCATED'
    DEALLOC_STATE = 'DEALLOCATED'
    SUBMIT_STATE = 'SUBMITTED'
    SENT_STATE = 'SENT'
    STATES = [ALLOC_STATE, SUBMIT_STATE, SENT_STATE, DEALLOC_STATE]
    ALLOC_SEARCH_STRS = ('allocated', 'allocated per RPC request received')
    SUBMIT_SEARCH_STR = 'submitted'
    SENT_SEARCH_STR = 'sent'
    DEALLOC_SEARCH_STR = 'decref to 0'
    SEARCH_STRS = [ALLOC_SEARCH_STRS, SUBMIT_SEARCH_STR, SENT_SEARCH_STR,
                   DEALLOC_SEARCH_STR]
    VERBOSE_STATE_TRANSITIONS = False
    VERBOSE_LOG = True

    def __init__(self, fname, output_stream):
        self.set_log(output_stream)
        self.input_file = fname
        #search for all process ids in logfile with multiple instances logged
        with open(self.input_file, 'r') as f:
            pids = []
            for line in f:
                fields = line.split()
                if len(fields[0]) != 17:
                    continue
                pid = int(fields[2][5:-1])
                if pid not in pids:
                    pids.append(pid)

        #index_multiprocess will determine which PID to trace
        #(ie index 0 will be assigned the first PID found in logs)
        self.pids = sorted(pids)

    def _rpc_error_state_tracing(self, rpc, rpc_state, opcode):
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
                message = 'previous state: {0}'.format(self.rpc_dict[rpc])
        else:
            if rpc_state == self.ALLOC_STATE:
                status = 'SUCCESS'
            else:
                status = 'WARN'
                message = "no alloc'd state registered"

            self.rpc_op_dict[rpc] = opcode

        if rpc_state == self.DEALLOC_STATE:
            del self.rpc_dict[rpc]
            del self.rpc_op_dict[rpc]
        else:
            self.rpc_dict[rpc] = rpc_state
        return (status, message)

    def _rpc_tabulate(self):
        """Use tabulate pkg to formulate table of opcodes and RPC states"""
        op_table = []
        alloc_table = []
        dealloc_table = []
        submit_table = []
        sent_table = []
        errors = []

        (op_table, alloc_table, dealloc_table, sent_table, submit_table,
         errors) = self._rpc_tabulate_populate_lists()

        #Error checking and tabulate method called to create table outputs

        #check list lengths to make sure all RPC states are correct and
        #acounted for
        length = len(op_table)
        if any(len(l) != length for l in [alloc_table, submit_table, sent_table,
                                          dealloc_table]):
            errors.append('ERROR: Lengths of RPC state lists are not the same')

        #check if alloc'd and dealloc'd state transition totals are equal
        zipped_list = list(zip(op_table, alloc_table, dealloc_table))
        for x in zipped_list:
            if x[1] != x[2]:
                errors.append('ERROR: Opcode {0}: Alloc\'d Total = {1}, '
                              'Dealloc\'d Total = {2}'.format(x[0], x[1], x[2]))

        table = zip(op_table, alloc_table, submit_table, sent_table,
                    dealloc_table)
        headers = ['OPCODE']
        headers.extend(self.STATES)
        return (tabulate.tabulate(table, headers=headers), errors)

    def _rpc_tabulate_populate_lists(self):
        """Create and populate lists for all RPC states for table ouput of
        Opcode vs State Transition Counts"""
        op_table = []
        alloc_table = []
        dealloc_table = []
        submit_table = []
        sent_table = []
        errors = []
        alloc_total = 0
        dealloc_total = 0
        submit_total = 0
        sent_total = 0

        #create a dictionary of opcode and number of state types present
        newlist = [i[0] for i in sorted(Counter(self.op_state_list))]
        count_list = {}
        for value, count in sorted(Counter(newlist).items()):
            count_list[value] = count

        for (op, state), count in sorted(Counter(self.op_state_list).items()):
            if op not in op_table or op == op_table[-1]:
                if op not in op_table:
                    op_table.append(op)

                if state == self.ALLOC_STATE:
                    alloc_table.append(count)
                    alloc_total += 1
                elif state == self.DEALLOC_STATE:
                    dealloc_table.append(count)
                    dealloc_total += 1
                elif state == self.SUBMIT_STATE:
                    submit_table.append(count)
                    submit_total += 1
                elif state == self.SENT_STATE:
                    sent_table.append(count)
                    sent_total += 1

                if op == op_table[-1]:
                    #"end" is determined by how many state types are present
                    #per opcode (states are sorted)
                    #(4 = ALLOC, DEALLOC, SENT, SUBMIT) - end state is submit
                    #(2 = ALLOC, DEALLOC) - end state is dealloc
                    if (state == self.SUBMIT_STATE and \
                        count_list[op] == 4) or \
                       (state == self.DEALLOC_STATE and \
                        count_list[op] == 2):

                        #zero out blank table entries and reset counters
                        if alloc_total == 0:
                            alloc_table.append(alloc_total)
                        if dealloc_total == 0:
                            dealloc_table.append(dealloc_total)
                        if submit_total == 0:
                            submit_table.append(submit_total)
                        if sent_total == 0:
                            sent_table.append(sent_total)
                        alloc_total = 0
                        dealloc_total = 0
                        submit_total = 0
                        sent_total = 0

            else:
                errors.append('ERROR: RPC {0} found but does not match previous'
                              ' opcode {1}'.format(op, op_table[-1]))

        return (op_table, alloc_table, dealloc_table, sent_table, submit_table,
                errors)

    def rpc_reporting(self, index_multiprocess=None):
        """RPC reporting for RPC state machine"""

        if index_multiprocess is not None:
            #index_multiprocess is None if there is only one process being
            #traced, otherwise first index should start at 0
            pid_to_trace = self.pids[index_multiprocess]
            self._rpc_reporting_pid(pid_to_trace)
            return

        if len(self.pids) != 1:
            self.error_output("Multiprocess log file")
            return

        self._rpc_reporting_pid(self.pids[0])

    def _rpc_reporting_pid(self, pid_to_trace):
        """RPC reporting for RPC state machine, for mutiprocesses"""
        self.rpc_dict = {}
        self.op_state_list = []
        self.rpc_op_dict = {}

        pid_str = "CaRT[%d]" % pid_to_trace

        self.normal_output('\nCaRT RPC Reporting:\nLogfile: {0}, '
                           'PID: {1}\n'.format(self.input_file,
                                               pid_to_trace))

        f = open(self.input_file, 'r')
        ort = []

        for line in f:
            if any(s in line for s in self.SEARCH_STRS[0]) or \
               any(s in line for s in self.SEARCH_STRS[1:]):
                rpc_state = None
                rpc = None
                opcode = None
                fields = line.strip().split()
                if fields[2] != pid_str:
                    continue

                #remove ending punctuation from log msg
                translator = str.maketrans('', '', string.punctuation)
                str_match = fields[-1].translate(translator)
                if str_match == '0': #'decref to 0'
                    str_match = ' '.join(fields[-3:]).translate(translator)
                elif str_match == 'received': #'allocated per RPC request
                    # received'
                    str_match = ' '.join(fields[-5:]).translate(translator)

                rpc = fields[8]

                if any(s in str_match for s in self.SEARCH_STRS[0]):
                    rpc_state = self.ALLOC_STATE
                    opcode = fields[10][:-2]
                elif str_match == self.SEARCH_STRS[-1]:
                    rpc_state = self.DEALLOC_STATE
                    opcode = fields[10][:-2]
                #opcode not printed in submitted/sent log messages;
                #use rpc_op_dict{} to store RPC and opcode
                elif str_match == self.SEARCH_STRS[1]:
                    rpc_state = self.SUBMIT_STATE
                    opcode = self.rpc_op_dict.get(rpc, None)
                elif str_match == self.SEARCH_STRS[2]:
                    rpc_state = self.SENT_STATE
                    opcode = self.rpc_op_dict.get(rpc, None)

                if rpc and opcode and rpc_state:
                    self.op_state_list.append((opcode, rpc_state))
                    (state, extra) = self._rpc_error_state_tracing(rpc,
                                                                   rpc_state,
                                                                   opcode)
                    function_name = fields[6]
                    if self.VERBOSE_STATE_TRANSITIONS or state != 'SUCCESS':
                        ort.append([state,
                                    rpc,
                                    rpc_state,
                                    opcode,
                                    function_name,
                                    extra])

        if ort:
            str_out = tabulate.tabulate(ort, headers=['STATE',
                                                      'RPC',
                                                      'STATE',
                                                      'Op',
                                                      'Function',
                                                      'Extra'])
            self.normal_output('RPC State Transitions:')
            self.normal_output(str_out)
            self.normal_output('')

        output_rpcs = []
        output_rpcs.append('Opcode State Transition Tally:')
        (ret_str, errors) = self._rpc_tabulate()
        output_rpcs.append(ret_str)
        output_rpcs.extend(errors)

        self.list_output(output_rpcs)
        f.close()

    #************ Descriptor Tracing Methods (IOF_TRACE macros) **********

    def descriptor_rpc_trace(self):
        """Parses twice thru log to create a hierarchy of descriptors and also
           a dict storing all RPCs tied to a descriptor"""
        self.trace_dict = {}
        self.desc_dict = {}
        log_path = self.input_file
        self.normal_output('\nIOF Descriptor/RPC Tracing:\n'
                           'Logfile: {0}'.format(log_path))


        self._descriptor_error_state_tracing(log_path)

        with open(log_path, 'r') as f:
            for line in f:
                if "TRACE" in line and "Registered new" in line:
                    #register a new descriptor/object in the log hierarchy
                    fields = line.strip().split()
                    new_obj = fields[7].strip().split('(')[1].strip().\
                              split(')')[0]
                    parent = fields[-1]
                    obj_type_l = fields[-3]
                    obj_type = obj_type_l[1:-1]
                    if new_obj not in self.trace_dict:
                        self.trace_dict.setdefault(new_obj, []).\
                                                   append((obj_type, parent))
                        self.desc_dict[new_obj] = []
                    else:
                        #add all reused descriptors to the dict, and append
                        #iteration number
                        reuse_iter = 1
                        descriptor_iter = '{0}_{1}'.format(new_obj, reuse_iter)
                        while descriptor_iter in self.trace_dict:
                            reuse_iter += 1
                            descriptor_iter = '{0}_{1}'.format(new_obj,
                                                               reuse_iter)
                        self.trace_dict.setdefault(descriptor_iter, []).\
                                                   append((obj_type, parent))
                        self.desc_dict[descriptor_iter] = []

        with open(log_path, 'r') as f:
            for line in f:
                if "TRACE" in line and "Alias" in line:
                    #create an alias for an already registered descriptor
                    fields = line.strip().split()
                    parent = fields[-1]
                    obj_type_l = fields[-3]
                    obj_type = obj_type_l[1:-1]
                    #find index of descriptor in order to append alias
                    #to correct index in the chance it is reused in the dict
                    index = self._find_reused_descriptor_index(log_path, line)
                    if index in self.trace_dict:
                        self.trace_dict.setdefault(index, []).\
                                                   append((obj_type, parent))
                    else:
                        self.error_output('{0} cannot be an alias, not '
                                          'registered'.format(index))

        with open(log_path, 'r') as f:
            for line in f:
                if "TRACE" in line and "Link" in line:
                    #register RPCs tied to given handle
                    fields = line.strip().split()
                    rpc = fields[7].strip().split('(')[1].strip().split(')')[0]
                    rpc_type = fields[9]
                    #find index of descriptor in order to append rpc
                    #to correct index in the chance it is reused in the dict
                    index = self._find_reused_descriptor_index(log_path, line)
                    if index in self.desc_dict:
                        self.desc_dict[index].append((rpc, rpc_type))
                    else:
                        self.error_output('Descriptor {0} is not present'.\
                                          format(index))

    def _rpc_trace_output_hierarchy(self, descriptor):
        """Prints full TRACE hierarchy for a given descriptor"""
        output = []
        #append all descriptors/rpcs in hierarchy for log dump
        traces_for_log_dump = []
        trace = descriptor
        #checking if descriptor is an rpc, in which case it is only linked
        #to other descriptors with TRACE
        rpc_type_list = [v for (k, v) in self.desc_dict.items()]
        desc_is_rpc = bool(descriptor not in self.trace_dict and \
                           [item for sublist in rpc_type_list \
                            for item in sublist if item[0] == descriptor])

        if desc_is_rpc: #tracing an rpc
            output.append('No hierarchy available - descriptor is an RPC')
            traces_for_log_dump.append(trace)
        else:
            #list of all alias descriptors whose path has not been traced
            yet_to_trace = [trace]
            while yet_to_trace:
                #start the trace at the next alias (or first descriptor if first
                #run thru
                trace = yet_to_trace[0]
                traces_for_log_dump.append(trace)
                yet_to_trace.remove(trace)
                #append an extra line
                output.append(' ')
                while trace in self.trace_dict:
                    #check if descriptor has registered alias(es)
                    alias_parents = [x[1] for x in self.trace_dict[trace]]
                    aliases = [x[0] for x in self.trace_dict[trace]]
                    #even if descriptor is not an alias, aliases[] will still
                    #contain one value of the descriptor type
                    if not aliases:
                        continue
                    trace_type = aliases[0]
                    aliases.remove(trace_type)
                    output.append('{0}: {1}'.format(trace_type, trace))
                    #print out all linked rpcs
                    for i in self.desc_dict[trace]:
                        traces_for_log_dump.append(i[0])
                        output.append('\t{0} {1}'.format(i[1], i[0]))
                    trace = alias_parents[0]
                    traces_for_log_dump.append(trace)
                    alias_parents.remove(trace)
                    for x, i in enumerate(aliases):
                        output.append('\t(alias) {0} linked to {1}'.\
                                      format(aliases[x], alias_parents[x]))
                        yet_to_trace.append(alias_parents[x])

        if not output:
            self.error_output('Descriptor {0} not currently registered or '
                              'linked'.format(descriptor))
        else:
            output.insert(0, '\nDescriptor Hierarchy ({0}):'.format(descriptor))
            self.list_output(output)

        return traces_for_log_dump

    def _rpc_trace_output_logdump(self, traces_for_log_dump):
        """Prints all log messages relating to the given descriptor or any
           pointer in the descriptor's hierarchy"""
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
        rpc_type_list = [v for (k, v) in self.desc_dict.items()]
        desc_is_rpc = bool(trace not in self.trace_dict and \
                           [item for sublist in rpc_type_list \
                            for item in sublist if item[0] == trace])

        output = []
        with open(self.input_file, 'r') as f:
            output.append('\nLog dump for descriptor hierarchy ({0}):'\
                          .format(trace))
            for line in f:
                desc_log_marked = False
                if desc_is_rpc: #tracing an rpc
                    if 'TRACE' in line:
                        fields = line.strip().split()
                        rpc = fields[7].strip().split('(')[1].strip().\
                              split(')')[0]
                        if rpc == desc:
                            output.append('MARK: {0}'.\
                                          format(' '.join(line.splitlines())))
                            desc_log_marked = True
                elif 'TRACE' in line and 'Registered new' in line:
                    fields = line.strip().split()
                    rpc = fields[7].strip().split('(')[1].strip().\
                          split(')')[0]
                    if rpc != desc:
                        #print the remaining non-relevant log messages
                        output.append(' '.join(line.splitlines()))
                        continue
                    if location is None:#tracing a unique descriptor
                        output.append('MARK: {0}'.\
                                      format(' '.join(line.splitlines())))
                        for nxtline in f:#start log dump after "registered" log
                            desc_log_marked = False
                            if 'TRACE' in nxtline:
                                fields = nxtline.strip().split()
                                rpc = fields[7].strip().split('(')[1].\
                                      strip().split(')')[0]
                                if any(s == rpc for s in descriptors):
                                    output.append('MARK: {0}'.\
                                                  format(' '.\
                                                         join(nxtline.\
                                                              splitlines())))
                                    desc_log_marked = True
                            if not desc_log_marked and self.VERBOSE_LOG:
                                #print the remaining non-relevant log messages
                                output.append(' '.join(nxtline.splitlines()))
                        self.list_output(output)
                        return
                    #start the log dump where the specific reused descriptor
                    #is registered, logging will include all instances of
                    #this descriptor after
                    if position_desc_cnt == location:
                        output.append('MARK: {0}'.\
                                      format(' '.join(line.splitlines())))
                        for nxtline in f:
                            desc_log_marked = False
                            if 'TRACE' in nxtline:
                                fields = nxtline.strip().split()
                                rpc = fields[7].strip().split('(')[1].\
                                      strip().split(')')[0]
                                if any(s == rpc for s in descriptors):
                                    output\
                                        .append('MARK: {0}'.\
                                                format(' '.join(nxtline.\
                                                                splitlines())))
                                    desc_log_marked = True
                            if not desc_log_marked and self.VERBOSE_LOG:
                                #print the remaining non-relevant log mesgs
                                output.append(' '.join(nxtline.splitlines()))
                        self.list_output(output)
                        return
                    position_desc_cnt += 1

                if not desc_log_marked and self.VERBOSE_LOG:
                    #print the remaining non-relevant log messages
                    output.append(' '.join(line.splitlines()))

        self.list_output(output)

    def rpc_trace_output(self, descriptor):
        """Dumps all RPCs tied to a descriptor, descriptor hierarchy, and all
           log messages related to descriptor"""
        missing_links = []
        output = []
        traces_for_log_dump = []
        self.normal_output('\n{0:<30}{1:<30}{2:<30}'.format('Descriptor',
                                                            'Type', 'Parent'))
        self.normal_output('{0:<30}{1:<30}{2:<30}'.format('----------', '----',
                                                          '------'))

        for key in sorted(self.trace_dict):
            for typ, par in self.trace_dict[key]:
                if par in self.trace_dict:
                    par_types = [x[0] for x in self.trace_dict[par]]
                    output.append('{0:<30}{1:<30}{2} [{3}]'.\
                                  format(key, typ, par,
                                         par_types))
                elif par == 'root':
                    output.append('{0:<30}{1:<30}{2} [None]'.\
                                  format(key, typ, par))
                else: #fail if missing parent/link
                    output.append('ERROR: {0:<30}{1:<30}{2} [None]'.\
                                  format(key, typ, par))
                    missing_links.append(par)

        self.list_output(output)
        output = []

        self.normal_output('\n\n{:<60}{:<30}'.format('Descriptor', 'RPCs'))
        self.normal_output('{:<60}{:<30}'.format('----------', '----'))
        for k, v in sorted(self.desc_dict.items()):
            if k in self.trace_dict:
                #reformat rpc list for better output display in table
                temp_v = []
                v_output = ''
                add_format = False
                for i in v:
                    if add_format:
                        temp_v.append('{:>65}:{:<15}'.format(i[0], i[1]))
                    else:
                        temp_v.append('{:>25}:{:<15}'.format(i[0], i[1]))
                        add_format = True
                if v:
                    v_output = '\n'.join(temp_v)
                type_field = str([x[0] for x in self.trace_dict[k]])
                output.append('{:<20}{:<25}{}'.\
                              format(k, type_field, v_output))
        self.list_output(output)

        #Hierarchy for given descriptor
        traces_for_log_dump = self._rpc_trace_output_hierarchy(descriptor)

        #Log dump for descriptor hierarchy
        self._rpc_trace_output_logdump(traces_for_log_dump)

        return missing_links

    def _descriptor_error_state_tracing(self, log_path):
        """Check for any descriptors that are not registered/deregistered"""
        self.normal_output('\nDescriptor State Transitions:')
        self.desc_state = {}
        with open(log_path, 'r') as f:
            output = []
            self.normal_output('{0:<30}{1:<20}{2:<20}\n{3:<30}{4:<20}{5:<20}'.\
                               format('Descriptor', 'State', 'Function',
                                      '----------', '-----', '--------'))
            for line in f:
                if 'TRACE' not in line:
                    continue

                state = None
                fields = line.strip().split()
                part = fields[7]
                start_idx = part.find('(')
                desc = part[start_idx+1:-1]
                if desc == '(nil)':
                    continue
                res = None

                if 'Registered new' in line:
                    state = 'Registered'
                    if desc in self.desc_state:
                        res = ('ERROR', state, 'previous state: {0}' \
                               .format(self.desc_state[desc]))
                    else:
                        res = ('SUCCESS', state)
                    self.desc_state[desc] = state
                #only for aliases, key for dict will now be "type" and value
                #the descriptor
                elif 'Alias' in line:
                    state = 'Alias'
                    obj_type_l = fields[-3]
                    obj_type = obj_type_l[1:-1]
                    if self.desc_state.get(desc, None) != 'Registered':
                        res = ('ERROR', state, 'Not registered')
                    else:
                        res = ('SUCCESS', state)
                    self.desc_state[obj_type] = desc
                elif 'Link' in line:
                    state = 'Linked'
                    if self.desc_state.get(desc, None) == 'Linked' or \
                       desc not in self.desc_state:
                        res = ('SUCCESS', state)
                    else:
                        res = ('ERROR', state)
                    self.desc_state[desc] = state
                elif 'Deregistered' in line:
                    state = 'Deregistered'
                    if self.desc_state.get(desc, None) == 'Registered':
                        del self.desc_state[desc]
                        res = ('SUCCESS', state)
                        #check for aliases to also de-register
                        for k, v in list(self.desc_state.items()):
                            if v == desc:
                                del self.desc_state[k]
                    elif self.desc_state.get(desc, None) == 'Linked':
                        res = ('ERROR', state, 'Linked RPC')
                    else:
                        res = ('ERROR', state)
                if not res:
                    continue

                function_name = part[:start_idx]
                if self.VERBOSE_STATE_TRANSITIONS or res[0] != 'SUCCESS':
                    desc = '{0}: {1}'.format(res[0], desc)
                    if len(res) == 2:
                        output.append('{0:<30}{1:<20}{2}()' \
                                      .format(desc,
                                              res[1],
                                              function_name))
                    else:
                        output.append('{0:<30}{1:<20}{2}() ({3})' \
                                      .format(desc,
                                              res[1],
                                              function_name,
                                              res[2]))

            self.list_output(output)

        #check if all descriptors are deregistered
        for d in list(self.desc_state):
            if self.desc_state.get(d, None) == 'Registered':
                self.error_output('{0} is not Deregistered'.format(d))
            elif self.desc_state.get(d, None) == 'Linked':
                #currently no "unlinked" state, CaRT RPC debugging is done
                #prior to this
                del self.desc_state[d]
        #final check to make sure all registered descriptors are deleted
        for d, state in self.desc_state.items():
            self.error_output('{0}:{1} not deregistered from state'.\
                              format(d, state))

    def _find_reused_descriptor_index(self, log_dir, log_line):
        """Iterate over the log file given the line where the descriptor is
        created to find the position count of the descriptor (suffix appended to
        descriptor key in dict for reused descriptors)
        Return the index of the descriptor"""
        descriptor = None
        position_desc_cnt = 0
        with open(log_dir, 'r') as f:
            for line in f:
                if log_line != line:
                    continue
                fields = line.strip().split()
                if "Link" in line:
                    descriptor = fields[-1]
                else:
                    descriptor = fields[7].strip().split('(')[1].strip().\
                                 split(')')[0]
                reused_descs = [v for k, v in self.trace_dict.items() \
                                if descriptor in k]
                if len(reused_descs) > 1:
                    for nxtline in f:
                        if 'TRACE' in nxtline and 'Registered new' in \
                           nxtline:
                            fields = nxtline.strip().split()
                            new_obj = fields[7].strip().split('(')[1].\
                                      strip().split(')')[0]
                            if new_obj == descriptor:
                                position_desc_cnt += 1

                pos = len(reused_descs) - position_desc_cnt - 1
                if pos > 0:
                    descriptor = '{0}_{1}'.\
                                 format(descriptor,
                                        len(reused_descs) - \
                                        position_desc_cnt - 1)
                return descriptor

        self.error_output('Reused descriptor index not found')
        return None

    def descriptor_to_trace(self):
        """Find the file handle to use for descriptor tracing:
        if an error or warning is found in trace logs, use that descriptor,
        otherwise use first fuse op instance in logs"""
        descriptor = None
        position_desc_cnt = 0
        fuse_file = os.path.join('src', 'ioc', 'ops')
        with open(self.input_file, 'r') as f:
            for line in f:
                if 'TRACE' in line:
                    if 'ERR' in line or 'WARN' in line:
                        fields = line.strip().split()
                        descriptor = fields[7].strip().split('(')[1].strip().\
                                     split(')')[0]
                        self.warning_output('Tracing descriptor {0} with '
                                            'error/warning'.format(descriptor))
                        reused_descs = [v for k, v in self.trace_dict.items() \
                                        if descriptor in k]
                        if len(reused_descs) > 1:
                            for nxtline in f:
                                if 'TRACE' in nxtline and 'Registered new' in \
                                    nxtline:
                                    fields = nxtline.strip().split()
                                    new_obj = fields[7].strip().split('(')[1].\
                                              strip().split(')')[0]
                                    if new_obj == descriptor:
                                        position_desc_cnt += 1

                        pos = len(reused_descs) - position_desc_cnt - 1
                        if pos > 0:
                            descriptor = '{0}_{1}'.\
                                          format(descriptor,
                                                 len(reused_descs) - \
                                                 position_desc_cnt - 1)
                        return descriptor
        with open(self.input_file, 'r') as f:
            for line in f:
                if fuse_file in line and 'TRACE' in line:
                    fields = line.strip().split()
                    descriptor = fields[7].strip().split('(')[1].strip().\
                                 split(')')[0]
                    return descriptor
        self.error_output('Descriptor not found to trace')
        return None
