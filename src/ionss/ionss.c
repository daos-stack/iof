
#include <stdio.h>
#include "version.h"
#include "log.h"
#include <mercury.h>
#include <process_set.h>

int main(void)
{
	char *uri;
	struct mcl_set *set;
	struct mcl_state *proc_state;
	na_class_t *na_class = NULL;

	char *version = iof_get_version();

	iof_log_init("ionss");

	IOF_LOG_INFO("IONSS version: %s", version);

	proc_state = mcl_init(&uri);
	if (proc_state == NULL) {
		IOF_LOG_ERROR("mcl_init() failed.");
		return 1;
	}
	na_class = NA_Initialize(uri, NA_TRUE);

	free(uri);

	mcl_startup(proc_state, na_class, "ionss", 1, &set);
	IOF_LOG_INFO("name %s size %d rank %d is_local %d is_service %d",
		     set->name, set->size, set->self, set->is_local,
		     set->is_service);

	mcl_finalize(proc_state);
	NA_Finalize(na_class);

	iof_log_close();

	return 0;
}
