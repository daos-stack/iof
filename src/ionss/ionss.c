#include <stdio.h>
#include <unistd.h>
#include "version.h"
#include "log.h"
#include <mercury.h>
#include <process_set.h>
#include "iof_common.h"

int main(void)
{
	struct mcl_set *set;
	struct mcl_state *proc_state;
	hg_id_t rpc_id;

	char *version = iof_get_version();

	iof_log_init("ionss");
	IOF_LOG_INFO("IONSS version: %s", version);

	proc_state = mcl_init();
	if (proc_state == NULL) {
		IOF_LOG_ERROR("mcl_init() failed.");
		return 1;
	}
	rpc_id = HG_Register_name(proc_state->hg_class, "Projection_query",
			NULL, iof_query_out_proc_cb, iof_query_handler);
	IOF_LOG_INFO("Id registered on server:%d", rpc_id);
	mcl_startup(proc_state, "ionss", 1, &set);
	IOF_LOG_INFO("name %s size %d rank %d is_local %d is_service %d",
		     set->name, set->size, set->self, set->is_local,
		     set->is_service);
	mcl_finalize(proc_state);
	iof_log_close();

	return 0;
}
