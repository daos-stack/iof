#include "log.h"

int iof_log_handle;

void iof_log_init(const char *component)
{
	iof_log_handle = mcl_log_open(component, false);
}

void iof_log_close(void)
{
	mcl_log_close(iof_log_handle);
}
