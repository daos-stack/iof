/* Common test logging file */

#include <stdio.h>
#include <stdlib.h>

#include "iof_test_log.h"

int iof_testlog_handle;
char log_level[8];

void iof_testlog_init(const char *component)
{
	sprintf(log_level, "%d", MCL_DEBUG_LEVEL);
	setenv("MCL_LOG_LEVEL",  log_level, 1);
	iof_testlog_handle = mcl_log_open(component, false);
}

void iof_testlog_close(void)
{
	mcl_log_close(iof_testlog_handle);
}
