#include <stdio.h>
#include "version.h"
#include "log.h"
#include <mercury.h>
#include <inttypes.h>
#include <process_set.h>
#include <mercury.h>
#include <errno.h>
#include "process_set.h"
#include "iof_common.h"
#include "mcl_event.h"


struct query_reply {
	struct mcl_event event;
	int err_code;
	struct iof_psr_query *query;
};

static hg_return_t query_callback(const struct hg_cb_info *info)
{
	struct query_reply *reply;
	int ret;

	reply = info->arg;
	ret = HG_Get_output(info->info.forward.handle, reply->query);
	if (ret != HG_SUCCESS)
		IOF_LOG_ERROR("Cant unpack output of RPC");
	reply->err_code = ret;
	mcl_event_set(&reply->event);
	return HG_SUCCESS;
}

/*Send RPC to PSR to get information about projected filesystems*/
static int ioc_get_projection_info(struct mcl_state *state, hg_addr_t psr_addr,
			struct iof_psr_query *query, hg_id_t rpc_id)
{
	hg_handle_t handle;
	int ret;
	struct query_reply reply = {0};

	reply.query = query;
	mcl_event_clear(&reply.event);

	ret = HG_Create(state->hg_context, psr_addr, rpc_id, &handle);
	if (ret != HG_SUCCESS) {
		IOF_LOG_ERROR("Handle not created");
		return ret;
	}
	ret = HG_Forward(handle, query_callback, &reply, NULL);
	if (ret != HG_SUCCESS) {
		IOF_LOG_ERROR("Could not send RPC tp PSR");
		return ret;
	}
	while (!mcl_event_test(&reply.event))
		sched_yield();
	ret = HG_Destroy(handle);
	if (ret != HG_SUCCESS)
		IOF_LOG_ERROR("Could not destroy handle");
	return ret;
}

int main(void)
{
	char *uri;
	struct mcl_set *cnss_set;
	struct mcl_state *cnss_state;
	na_class_t *na_class = NULL;
	struct mcl_set *ionss_set;
	hg_addr_t psr_addr;
	int ret;
	int i;
	hg_id_t rpc_id;
	struct iof_psr_query query = {0};
	struct iof_fs_info *tmp = NULL;

	char *version = iof_get_version();
	iof_log_init("cnss");

	IOF_LOG_INFO("CNSS version: %s", version);

	cnss_state = mcl_init(&uri);
	if (cnss_state == NULL) {
		IOF_LOG_ERROR("mcl_init() failed");
		return 1;
	}
	na_class = NA_Initialize(uri, NA_FALSE);
	if (na_class == NULL) {
		IOF_LOG_ERROR("NA Class not initialised");
		return 1;
	}
	free(uri);
	mcl_startup(cnss_state, na_class, "cnss", 0, &cnss_set);
	ret = mcl_attach(cnss_state, "ionss", &ionss_set);
	if (ret != MCL_SUCCESS) {
		IOF_LOG_ERROR("Attach to IONSS Failed");
		return 1;
	}
	ret = mcl_lookup(ionss_set, ionss_set->psr_rank, na_class, &psr_addr);
	if (ret != MCL_SUCCESS) {
		IOF_LOG_ERROR("PSR Address lookup failed");
		return 1;
	}

	rpc_id = HG_Register_name(cnss_state->hg_class, "Projection_query",
			NULL, iof_query_out_proc_cb, iof_query_handler);
	IOF_LOG_INFO("Id registered on CNSS:%d", rpc_id);
	/*Query PSR*/
	ret = ioc_get_projection_info(cnss_state, psr_addr, &query, rpc_id);
	if (ret != 0)
		IOF_LOG_ERROR("Query operation failed");
	else {
		for (i = 0; i < query.num; i++) {
			tmp = &query.list[i];
			if (tmp->mode == 0)
				IOF_LOG_INFO("Filesystem mode: Private");
			IOF_LOG_INFO("Projected Mount %s", tmp->mnt);
			IOF_LOG_INFO("Filesystem ID %"PRIu64, tmp->id);
		}
	}
	if (query.list != NULL)
		free(query.list);

	mcl_detach(cnss_state, ionss_set);
	mcl_finalize(cnss_state);
	NA_Finalize(na_class);
	iof_log_close();
	return ret;
}
