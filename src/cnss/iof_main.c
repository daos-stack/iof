
#include <inttypes.h>

#include "iof_common.h"
#include "iof_plugin.h"
#include "iof.h"
#include "log.h"

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
static int ioc_get_projection_info(struct mcl_context *mcl_context,
				   hg_addr_t psr_addr,
				   struct iof_psr_query *query, hg_id_t rpc_id)
{
	hg_handle_t handle;
	int ret;
	struct query_reply reply = {0};

	reply.query = query;
	mcl_event_clear(&reply.event);

	ret = HG_Create(mcl_context->context, psr_addr, rpc_id, &handle);
	if (ret != HG_SUCCESS) {
		IOF_LOG_ERROR("Handle not created");
		return ret;
	}
	ret = HG_Forward(handle, query_callback, &reply, NULL);
	if (ret != HG_SUCCESS) {
		IOF_LOG_ERROR("Could not send RPC tp PSR");
		return ret;
	}
	mcl_progress(mcl_context, &reply.event);
	ret = HG_Destroy(handle);
	if (ret != HG_SUCCESS)
		IOF_LOG_ERROR("Could not destroy handle");
	return ret;
}

int iof_reg(void *foo, struct mcl_state *state, struct cnss_plugin_cb *cb,
	    size_t cb_size)
{
	struct cnss_state *cnss_state = (struct cnss_state *)foo;

	cnss_state->mcl_state = state;
	cnss_state->context = mcl_get_context(state);
	cnss_state->projection_query = HG_Register_name(state->hg_class,
							"Projection_query",
							NULL,
							iof_query_out_proc_cb,
							iof_query_handler);
	IOF_LOG_INFO("Id registered on CNSS: %d", cnss_state->projection_query);

	return 0;
}

int iof_post_start(void *foo, struct mcl_set *set)
{
	struct cnss_state *state = (struct cnss_state *)foo;
	struct iof_psr_query query = {0};
	hg_addr_t psr_addr;
	int ret;
	int i;

	ret = mcl_lookup(set, set->psr_rank, state->context,
			 &psr_addr);
	if (ret != MCL_SUCCESS) {
		IOF_LOG_ERROR("PSR Address lookup failed");
		return -11;
	}

	/*Query PSR*/
	ret = ioc_get_projection_info(state->context, psr_addr, &query,
				      state->projection_query);
	if (ret != 0) {
		IOF_LOG_ERROR("Query operation failed");
	} else {
		for (i = 0; i < query.num; i++) {
			struct iof_fs_info *tmp = &query.list[i];

			if (tmp->mode == 0)
				IOF_LOG_INFO("Filesystem mode: Private");
			IOF_LOG_INFO("Projected Mount %s", tmp->mnt);
			IOF_LOG_INFO("Filesystem ID %" PRIu64, tmp->id);
		}
	}
	if (query.list)
		free(query.list);

	return 0;
}

void iof_flush(void *cbaddr)
{
	IOF_LOG_INFO("Called iof_flush with %p", cbaddr);
}

void iof_finish(void *cbaddr)
{
	IOF_LOG_INFO("Called iof_finish with %p", cbaddr);
	free(cbaddr);
}

struct cnss_plugin self = {.name            = "iof",
			   .version         = CNSS_PLUGIN_VERSION,
			   .require_service = 0,
			   .start           = iof_reg,
			   .post_start      = iof_post_start,
			   .flush           = iof_flush,
			   .finish          = iof_finish};

int iof_plugin_init(struct cnss_plugin **fns, size_t *size)
{
	*size = sizeof(struct cnss_plugin);

	self.handle = calloc(1, sizeof(struct cnss_state));
	if (!self.handle)
		return -1;

	*fns = &self;
	return 0;
}

