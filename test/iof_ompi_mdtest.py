#!/usr/bin/env python3
"""
test runner test

"""

import os
import logging
#pylint: disable=import-error
import NodeControlRunner
#pylint: enable=import-error


class TestMdtest(object):
    """Simple python test"""
    def __init__(self, test_info=None, log_base_path=None):
        self.test_info = test_info
        self.info = test_info.info
        self.log_dir_base = log_base_path
        self.logger = logging.getLogger("TestRunnerLogger")

    def useLogDir(self, log_path):
        """create the log directory name"""
        self.log_dir_base = log_path

    def test_mdtest_ompi(self):
        """Simple ping"""
        proc_rtn = 1
        self.logger.info("Test mdtest")
        testname = self.test_info.get_test_info('testName')
        testlog = os.path.join(self.log_dir_base, testname)
        nodes = NodeControlRunner.NodeControlRunner(testlog, self.test_info)
        prefix = self.test_info.get_defaultENV('IOF_OMPI_BIN', "")
        cmd_list = nodes.start_cmd_list(self.log_dir_base, testname, prefix)
        self.logger.debug("CMD: %s", str(cmd_list))
        nodes.add_nodes(cmd_list, 'IOF_TEST_CN')
        self.logger.debug("nodes added: %s", str(cmd_list))
        env_vars = {}
        env_vars['LD_LIBRARY_PATH'] = \
            self.test_info.get_defaultENV('LD_LIBRARY_PATH')
        nodes.add_env_vars(cmd_list, env_vars)
        self.logger.debug("env added: %s", str(cmd_list))
        parameters = "-i 3 -I 10 -d {!s}/FS_1".format(
            self.test_info.get_defaultENV('CNSS_PREFIX'))
        self.logger.debug("ready to go: %s", str(cmd_list))
        cmdstr = "{!s}/mdtest".format(
            self.test_info.parameters_one("{mdtest_path}"))
        nodes.add_cmd(cmd_list, cmdstr, parameters)
        proc = nodes.start_process(cmd_list)
        if nodes.check_process(proc):
            proc_rtn = nodes.wait_process(proc)
        return proc_rtn
