/* Copyright (C) 2016-2018 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted for any purpose (including commercial purposes)
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the
 *    documentation and/or materials provided with the distribution.
 *
 * 3. In addition, redistributions of modified forms of the source or binary
 *    code must carry prominent notices stating that the original code was
 *    changed and the date of the change.
 *
 *  4. All publications or advertising materials mentioning features or use of
 *     this software are asked, but not required, to acknowledge that it was
 *     developed by Intel Corporation and credit the contributors.
 *
 * 5. Neither the name of Intel Corporation, nor the name of any Contributor
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "iof_common.h"
#include "ioc.h"
#include "log.h"
#include "iof_ioctl.h"
#include "iof_pool.h"

struct query_cb_r {
	struct iof_tracker tracker;
	int err;
};

/* A generic callback for handling RPC replies from low-level FUSE RPCs.
 *
 * This can be used with RPCs where the reply type is either status_out
 * or if the reply type is NULL.
 */
void
ioc_ll_gen_cb(const struct crt_cb_info *cb_info)
{
	struct iof_status_out	*out = crt_reply_get(cb_info->cci_rpc);
	fuse_req_t		req = cb_info->cci_arg;
	int			ret;

	/* Local node was out of memory */
	if (cb_info->cci_rc == -DER_NOMEM)
		D_GOTO(out_err, ret = ENOMEM);

	/* Remote node was out of memory */
	if (cb_info->cci_rc == -DER_DOS)
		D_GOTO(out_err, ret = ENOMEM);

	/* All other errors with the RPC */
	if (cb_info->cci_rc != 0) {
		IOF_TRACE_INFO(req, "cci_rc is %d", cb_info->cci_rc);
		D_GOTO(out_err, ret = EIO);
	}

	if (out) {
		if (out->err)
			D_GOTO(out_err, ret = EIO);
		if (out->rc)
			D_GOTO(out_err, ret = out->rc);
	}

	if (req)
		IOF_FUSE_REPLY_ZERO(req);
	return;

out_err:
	if (req)
		IOF_FUSE_REPLY_ERR(req, ret);
}

int ioc_simple_resend(struct ioc_request *request)
{
	int rc;
	crt_rpc_t *resend_rpc;

	rc = crt_req_create(request->rpc->cr_ctx, NULL,
			    request->rpc->cr_opc, &resend_rpc);
	if (rc) {
		IOF_TRACE_ERROR(request, "Failed to create retry RPC");
		return EIO;
	}
	memcpy(resend_rpc->cr_input,
	       request->rpc->cr_input,
	       request->rpc->cr_input_size);
	/* Clean up old RPC */
	crt_req_decref(request->rpc);
	crt_req_decref(request->rpc);
	request->rpc = resend_rpc;
	/* Second addref is called in iof_fs_send */
	crt_req_addref(request->rpc);
	return iof_fs_send(request);
}

/* The eviction handler atomically updates the PSR of the group for which
 * this eviction occurred; or disables the group if no more PSRs remain.
 * It then locates all the projections corresponding to the group; if the
 * group was previously disabled, it marks them offline. Else, it migrates all
 * open handles to the new PSR. The PSR update and migration must be completed
 * before the callbacks for individual failed RPCs are invoked, so they may be
 * able to correctly re-target the RPCs and also use valid handles.
 */
static void ioc_eviction_cb(crt_group_t *group, d_rank_t rank, void *arg)
{
	int i, rc;
	d_rank_t evicted_psr, updated_psr, new_psr;
	d_rank_list_t *psr_list = NULL;
	struct iof_group_info *g = NULL;
	struct iof_state *iof_state = arg;
	struct iof_projection_info *fs_handle;

	IOF_TRACE_INFO(arg, "Eviction handler, Group: %s; Rank: %u",
		       group->cg_grpid, rank);

	for (i = 0; i < iof_state->num_groups; i++) {
		g = iof_state->groups + i;
		if (!strncmp(group->cg_grpid,
			     g->grp.dest_grp->cg_grpid,
			     CRT_GROUP_ID_MAX_LEN))
			break;
		g = NULL;
	}

	if (g == NULL) {
		IOF_TRACE_INFO(arg, "Not an ionss group: %s", group->cg_grpid);
		return;
	}

	rc = crt_lm_group_psr(group, &psr_list);
	IOF_TRACE_INFO(arg, "ListPtr: %p, List: %p, Ranks: %d",
		       psr_list, psr_list ? psr_list->rl_ranks : 0,
		       psr_list ? psr_list->rl_nr : -1);
	/* Sanity Check */
	if (rc || !psr_list || !psr_list->rl_ranks) {
		IOF_TRACE_ERROR(arg, "Invalid rank list, ret = %d", rc);
		if (!rc)
			rc = -EINVAL;
	}
	if (!rc && psr_list->rl_nr == 0) {
		/* No more ranks remaining. */
		IOF_TRACE_ERROR(arg, "No PSRs left to failover.");
		rc = -EHOSTDOWN;
	} else {
		new_psr = psr_list->rl_ranks[0];
	}

	if (psr_list != NULL)
		d_rank_list_free(psr_list);

	if (rc) {
		IOF_TRACE_WARNING(g, "Group %s disabled, rc=%d",
				  group->cg_grpid, rc);
		g->grp.enabled = false;
	} else {
		evicted_psr = rank;
		atomic_compare_exchange(&g->grp.pri_srv_rank,
					evicted_psr, new_psr);
		updated_psr = atomic_load_consume(&g->grp.pri_srv_rank);
		IOF_TRACE_INFO(g, "Updated: %d, Evicted: %d, New: %d",
			       updated_psr, evicted_psr, new_psr);
		/* TODO: This is needed for FUSE operations which are
		 * not yet using the failover codepath to send RPCs.
		 * This must be removed once all the FUSE ops have been
		 * ported. This code is not thread safe, so a FUSE call
		 * when this is being updated will cause a race condition.
		 */
		g->grp.psr_ep.ep_rank = new_psr;
	}

	d_list_for_each_entry(fs_handle, &iof_state->fs_list, link) {
		struct iof_file_handle *fh;
		struct iof_dir_handle *dh;

		if (fs_handle->proj.grp != &g->grp)
			continue;

		if (!g->grp.enabled || !IOF_HAS_FAILOVER(fs_handle->flags)) {
			IOF_TRACE_WARNING(fs_handle,
					  "Marking projection %d offline: %s",
					  fs_handle->fs_id,
					  fs_handle->mnt_dir.name);
			if (!g->grp.enabled)
				fs_handle->offline_reason = -rc;
			else
				fs_handle->offline_reason = EHOSTDOWN;
			continue;
		}

		/* Mark all local GAH entries as invalid */
		pthread_mutex_lock(&fs_handle->of_lock);
		d_list_for_each_entry(fh, &fs_handle->openfile_list, list) {
			IOF_TRACE_INFO(fs_handle,
				       "Invalidating file " GAH_PRINT_STR " %p",
				       GAH_PRINT_VAL(fh->common.gah), fh);
			H_GAH_SET_INVALID(fh);
		}
		pthread_mutex_unlock(&fs_handle->of_lock);
		pthread_mutex_lock(&fs_handle->od_lock);
		d_list_for_each_entry(dh, &fs_handle->opendir_list, list) {
			IOF_TRACE_INFO(fs_handle,
				       "Invalidating dir " GAH_PRINT_STR " %p",
				       GAH_PRINT_VAL(dh->gah), fh);
			H_GAH_SET_INVALID(dh);
		}
		pthread_mutex_unlock(&fs_handle->od_lock);

	}
}

/* Check if a remote host is down.  Used in RPC callback to check the cb_info
 * for permanent failure of the remote ep.
 */
#define IOC_HOST_IS_DOWN(CB_INFO) (((CB_INFO)->cci_rc == -DER_EVICTED) || \
					((CB_INFO)->cci_rc == -DER_OOG))

/* A generic callback function to handle completion of RPCs sent from FUSE,
 * and replay the RPC to a different end point in case the target has been
 * evicted (denoted by an "Out Of Group" return code). For all other failures
 * and in case of success, it invokes a custom handler (if defined).
 */
static void generic_cb(const struct crt_cb_info *cb_info)
{
	struct ioc_request *request = cb_info->cci_arg;
	struct iof_projection_info *fs_handle =
			request->cb->get_fsh(request);

	/* No Error */
	if (!cb_info->cci_rc)
		D_GOTO(done, 0);

	/* Errors other than evictions */
	if (!IOC_HOST_IS_DOWN(cb_info)) {
		D_GOTO(done, request->err = EIO);
	} else if (fs_handle->offline_reason) {
		IOF_TRACE_ERROR(request, "Projection Offline");
		D_GOTO(done, request->err = fs_handle->offline_reason);
	}

	if (request->cb->on_evict &&
	    !request->cb->on_evict(request))
		return;
done:
	if (request->cb->on_result)
		request->cb->on_result(request);
}

/*
 * Wrapper function that is called from FUSE to send RPCs. The idea is to
 * decouple the FUSE implementation from the actual sending of RPCs. The
 * FUSE callbacks only need to specify the inputs and outputs for the RPC,
 * without bothering about how RPCs are sent. This function is also intended
 * for abstracting various other features related to RPCs such as fail-over
 * and load balance, at the same time preventing code duplication.
 *
 * TODO: Deferred Execution: Check for PSR eviction and add requests
 * on open handles to a queue if migration of handles is in progress.
 */
int iof_fs_send(struct ioc_request *request)
{
	int rc;
	struct iof_projection_info *fs_handle =
			request->cb->get_fsh(request);

	request->ep.ep_tag = 0;
	request->ep.ep_rank = atomic_load_consume(
				&fs_handle->proj.grp->pri_srv_rank);
	request->ep.ep_grp = fs_handle->proj.grp->dest_grp;

	/* Defer clean up until the output is copied. */
	crt_req_addref(request->rpc);
	rc = crt_req_set_endpoint(request->rpc, &request->ep);
	if (rc)
		D_GOTO(err, 0);
	IOF_TRACE_INFO(request, "Sending RPC to PSR Rank %d",
		       request->rpc->cr_ep.ep_rank);
	rc = crt_req_send(request->rpc, generic_cb, request);
	if (rc)
		D_GOTO(err, 0);
	if (request->cb->on_send)
		request->cb->on_send(request);
	return 0;
err:
	IOF_TRACE_ERROR(request, "Could not send rpc, rc = %d", rc);
	crt_req_decref(request->rpc);
	return -EIO;
}

static void
query_cb(const struct crt_cb_info *cb_info)
{
	struct query_cb_r *reply = cb_info->cci_arg;

	reply->err = cb_info->cci_rc;
	iof_tracker_signal(&reply->tracker);
}

/*
 * Send RPC to PSR to get information about projected filesystems
 *
 * Returns CaRT error code.
 */
static int
get_info(struct iof_state *iof_state, struct iof_group_info *group,
	 crt_rpc_t **query_rpc)
{
	struct query_cb_r reply = {0};
	crt_rpc_t *rpc = NULL;
	int rc;

	*query_rpc = NULL;

	iof_tracker_init(&reply.tracker, 1);

	rc = crt_req_create(iof_state->iof_ctx.crt_ctx, &group->grp.psr_ep,
			    QUERY_PSR_OP, &rpc);
	if (rc != -DER_SUCCESS || !rpc) {
		IOF_TRACE_ERROR(iof_state,
				"failed to create query rpc request, rc = %d",
				rc);
		return rc;
	}

	IOF_TRACE_LINK(rpc, iof_state, "query_rpc");

	rc = crt_req_set_timeout(rpc, 5);
	if (rc != -DER_SUCCESS) {
		IOF_TRACE_ERROR(iof_state, "Could not set timeout, rc = %d",
				rc);
		crt_req_decref(rpc);
		return rc;
	}

	/* decref in query_projections */
	crt_req_addref(rpc);

	rc = crt_req_send(rpc, query_cb, &reply);
	if (rc != -DER_SUCCESS) {
		IOF_TRACE_ERROR(iof_state, "Could not send query RPC, rc = %d",
				rc);
		crt_req_decref(rpc);
		return rc;
	}

	/*make on-demand progress*/
	iof_wait(iof_state->iof_ctx.crt_ctx, &reply.tracker);

	if (reply.err) {
		IOF_TRACE_INFO(iof_state,
			       "Bad RPC reply %d %s",
			       reply.err,
			       d_errstr(reply.err));
		/* Matches decref in this function */
		crt_req_decref(rpc);

		return reply.err;
	}

	*query_rpc = rpc;

	return -DER_SUCCESS;
}

static int iof_uint_read(char *buf, size_t buflen, void *arg)
{
	uint *value = arg;

	snprintf(buf, buflen, "%u", *value);
	return 0;
}

static int iof_uint64_read(char *buf, size_t buflen, void *arg)
{
	uint64_t *value = arg;

	snprintf(buf, buflen, "%lu", *value);
	return 0;
}

/* Attach to a CaRT group
 *
 * Returns true on success.
 */
#define BUFSIZE 64
static bool
attach_group(struct iof_state *iof_state, struct iof_group_info *group)
{
	char buf[BUFSIZE];
	int ret;
	struct cnss_plugin_cb *cb;
	struct ctrl_dir *ionss_dir = NULL;
	d_rank_list_t *psr_list = NULL;

	cb = iof_state->cb;

	/* First check for the IONSS process set, and if it does not
	 * exist then * return cleanly to allow the rest of the CNSS
	 * code to run
	 */
	ret = crt_group_attach(group->grp_name, &group->grp.dest_grp);
	if (ret) {
		IOF_TRACE_ERROR(iof_state,
				"crt_group_attach failed with ret = %d", ret);
		return false;
	}

	ret = iof_lm_attach(group->grp.dest_grp, NULL);
	if (ret != 0) {
		IOF_TRACE_ERROR(iof_state,
				"Could not initialize failover, ret = %d", ret);
		return false;
	}

	ret = crt_group_config_save(group->grp.dest_grp, true);
	if (ret) {
		IOF_TRACE_ERROR(iof_state, "crt_group_config_save failed for "
				"ionss with ret = %d", ret);
		return false;
	}

	/*initialize destination endpoint*/
	group->grp.psr_ep.ep_grp = group->grp.dest_grp;
	ret = crt_lm_group_psr(group->grp.dest_grp, &psr_list);
	IOF_TRACE_INFO(group, "ListPtr: %p, List: %p, Ranks: %d",
		       psr_list, psr_list ? psr_list->rl_ranks : 0,
		       psr_list ? psr_list->rl_nr : 0);
	if (ret || !psr_list || !psr_list->rl_ranks || !psr_list->rl_nr) {
		IOF_TRACE_ERROR(group, "Unable to access "
				"PSR list, ret = %d", ret);
		return false;
	}
	/* First element in the list is the PSR */
	atomic_store_release(&group->grp.pri_srv_rank, psr_list->rl_ranks[0]);
	group->grp.psr_ep.ep_rank = psr_list->rl_ranks[0];
	group->grp.psr_ep.ep_tag = 0;
	d_rank_list_free(psr_list);
	IOF_TRACE_INFO(group, "Primary Service Rank: %d",
		       atomic_load_consume(&group->grp.pri_srv_rank));

	sprintf(buf, "%d", group->grp.grp_id);

	ret = cb->create_ctrl_subdir(iof_state->ionss_dir, buf,
				     &ionss_dir);
	if (ret != 0) {
		IOF_TRACE_ERROR(iof_state, "Failed to create control dir for "
				"ionss info (rc = %d)\n", ret);
		return false;
	}
	cb->register_ctrl_constant_uint64(ionss_dir, "psr_rank",
					  group->grp.psr_ep.ep_rank);
	cb->register_ctrl_constant_uint64(ionss_dir, "psr_tag",
					  group->grp.psr_ep.ep_tag);
	/* Fix this when we actually have multiple IONSS apps */
	cb->register_ctrl_constant(ionss_dir, "name", group->grp_name);

	group->grp.enabled = true;

	return true;
}

static bool ih_key_cmp(struct d_hash_table *htable, d_list_t *rlink,
		       const void *key, unsigned int ksize)
{
	const struct ioc_inode_entry *ie;
	const fuse_ino_t *ino = key;

	ie = container_of(rlink, struct ioc_inode_entry, list);

	return *ino == ie->ino;
}

static void ih_addref(struct d_hash_table *htable, d_list_t *rlink)
{
	struct ioc_inode_entry *ie;
	int oldref;

	ie = container_of(rlink, struct ioc_inode_entry, list);
	oldref = atomic_fetch_add(&ie->ref, 1);
	IOF_TRACE_DEBUG(ie, "addref to %u", oldref + 1);
}

static bool ih_decref(struct d_hash_table *htable, d_list_t *rlink)
{
	struct ioc_inode_entry *ie;
	int oldref;

	ie = container_of(rlink, struct ioc_inode_entry, list);
	oldref = atomic_fetch_sub(&ie->ref, 1);
	IOF_TRACE_DEBUG(ie, "decref to %u", oldref - 1);
	return oldref == 1;
}

static void ih_free(struct d_hash_table *htable, d_list_t *rlink)
{
	struct iof_projection_info *fs_handle = htable->ht_priv;
	struct ioc_inode_entry *ie;

	ie = container_of(rlink, struct ioc_inode_entry, list);

	IOF_TRACE_DEBUG(ie);
	if (ie->parent)
		drop_ino_ref(fs_handle, ie->parent);
	ie_close(fs_handle, ie);
	D_FREE(ie);
}

d_hash_table_ops_t hops = {.hop_key_cmp = ih_key_cmp,
			   .hop_rec_addref = ih_addref,
			   .hop_rec_decref = ih_decref,
			   .hop_rec_free = ih_free,
};

static void
dh_init(void *arg, void *handle)
{
	struct iof_dir_handle *dh = arg;

	dh->fs_handle = handle;
}

static bool
dh_reset(void *arg)
{
	struct iof_dir_handle *dh = arg;
	int rc;

	/* If there has been an error on the local handle, or readdir() is not
	 * exhausted then ensure that all resources are freed correctly
	 */
	if (dh->rpc)
		crt_req_decref(dh->rpc);
	dh->rpc = NULL;

	if (dh->open_req.rpc)
		crt_req_decref(dh->open_req.rpc);

	if (dh->close_req.rpc)
		crt_req_decref(dh->close_req.rpc);

	rc = crt_req_create(dh->fs_handle->proj.crt_ctx, NULL,
			    FS_TO_OP(dh->fs_handle, opendir),
			    &dh->open_req.rpc);
	if (rc || !dh->open_req.rpc)
		return false;

	rc = crt_req_create(dh->fs_handle->proj.crt_ctx, NULL,
			    FS_TO_OP(dh->fs_handle, closedir),
			    &dh->close_req.rpc);
	if (rc || !dh->close_req.rpc) {
		crt_req_decref(dh->open_req.rpc);
		return false;
	}
	return true;
}

static void
dh_release(void *arg)
{
	struct iof_dir_handle *dh = arg;

	crt_req_decref(dh->open_req.rpc);
	crt_req_decref(dh->close_req.rpc);
}

/* Create a getattr descriptor for use with mempool.
 *
 * Two pools of descriptors are used here, one for getattr and a second
 * for getfattr.  The only difference is the RPC id so the datatypes are
 * the same, as are the init and release functions.
 */
static void
fh_init(void *arg, void *handle)
{
	struct iof_file_handle *fh = arg;

	fh->fs_handle = handle;
}

static bool
fh_reset(void *arg)
{
	struct iof_file_handle *fh = arg;
	int rc;

	D_FREE(fh->name);

	if (fh->open_rpc) {
		crt_req_decref(fh->open_rpc);
		crt_req_decref(fh->open_rpc);
		fh->open_rpc = NULL;
	}

	if (fh->creat_rpc) {
		crt_req_decref(fh->creat_rpc);
		crt_req_decref(fh->creat_rpc);
		fh->creat_rpc = NULL;
	}

	if (fh->release_rpc) {
		crt_req_decref(fh->release_rpc);
		crt_req_decref(fh->release_rpc);
		fh->release_rpc = NULL;
	}

	fh->common.ep = fh->fs_handle->proj.grp->psr_ep;

	if (!fh->ie) {
		D_ALLOC_PTR(fh->ie);
		if (!fh->ie)
			return false;
		atomic_fetch_add(&fh->ie->ref, 1);
	}

	rc = crt_req_create(fh->fs_handle->proj.crt_ctx, &fh->common.ep,
			    FS_TO_OP(fh->fs_handle, open), &fh->open_rpc);
	if (rc || !fh->open_rpc) {
		D_FREE(fh->ie);
		return false;
	}

	rc = crt_req_create(fh->fs_handle->proj.crt_ctx, &fh->common.ep,
			    FS_TO_OP(fh->fs_handle, create), &fh->creat_rpc);
	if (rc || !fh->creat_rpc) {
		D_FREE(fh->ie);
		crt_req_decref(fh->open_rpc);
		return false;
	}

	rc = crt_req_create(fh->fs_handle->proj.crt_ctx, &fh->common.ep,
			    FS_TO_OP(fh->fs_handle, close), &fh->release_rpc);
	if (rc || !fh->release_rpc) {
		D_FREE(fh->ie);
		crt_req_decref(fh->open_rpc);
		crt_req_decref(fh->creat_rpc);
		return false;
	}

	crt_req_addref(fh->open_rpc);
	crt_req_addref(fh->creat_rpc);
	crt_req_addref(fh->release_rpc);
	return true;
}

static void
fh_release(void *arg)
{
	struct iof_file_handle *fh = arg;

	crt_req_decref(fh->open_rpc);
	crt_req_decref(fh->open_rpc);
	crt_req_decref(fh->creat_rpc);
	crt_req_decref(fh->creat_rpc);
	crt_req_decref(fh->release_rpc);
	crt_req_decref(fh->release_rpc);
	D_FREE(fh->ie);
}

static void
common_init(void *arg, void *handle)
{
	struct common_req *req = arg;

	req->fs_handle = handle;
}

/* Reset and prepare for use a getfattr descriptor */
static bool
gh_reset(void *arg)
{
	struct common_req *req = arg;
	int rc;

	req->request.req = NULL;

	if (req->request.rpc) {
		crt_req_decref(req->request.rpc);
		crt_req_decref(req->request.rpc);
		req->request.rpc = NULL;
	}

	rc = crt_req_create(req->fs_handle->proj.crt_ctx, NULL,
			    FS_TO_OP(req->fs_handle, getattr),
			    &req->request.rpc);
	if (rc || !req->request.rpc) {
		IOF_TRACE_ERROR(req, "Could not create request, rc = %u", rc);
		return false;
	}
	crt_req_addref(req->request.rpc);

	return true;
}

/* Reset and prepare for use a getfattr descriptor */
static bool
close_reset(void *arg)
{
	struct common_req *req = arg;
	int rc;

	if (req->request.rpc) {
		crt_req_decref(req->request.rpc);
		crt_req_decref(req->request.rpc);
		req->request.rpc = NULL;
	}

	rc = crt_req_create(req->fs_handle->proj.crt_ctx, NULL,
			    FS_TO_OP(req->fs_handle, close),
			    &req->request.rpc);
	if (rc || !req->request.rpc) {
		IOF_TRACE_ERROR(req, "Could not create request, rc = %u", rc);
		return false;
	}
	crt_req_addref(req->request.rpc);

	return true;
}

/* Destroy a descriptor which could be either getattr or close */
static void
common_release(void *arg)
{
	struct common_req *req = arg;

	crt_req_decref(req->request.rpc);
	crt_req_decref(req->request.rpc);
}

#define entry_init(type)					\
	static void type##_entry_init(void *arg, void *handle)	\
	{							\
		struct entry_req *req = arg;			\
								\
		req->fs_handle = handle;			\
		req->opcode = FS_TO_OP(dh->fs_handle, type);	\
	}
entry_init(lookup);
entry_init(mkdir);
entry_init(symlink);

static bool
entry_reset(void *arg)
{
	struct entry_req *req = arg;
	int rc;

	/* If this descriptor has previously been used the destroy the
	 * existing RPC
	 */
	if (req->request.rpc) {
		crt_req_decref(req->request.rpc);
		crt_req_decref(req->request.rpc);
		req->request.rpc = NULL;
	}

	/* Free any destination string on this descriptor.  This is only used
	 * for symlink to store the link target whilst the RPC is being sent
	 */
	D_FREE(req->dest);

	if (!req->ie) {
		D_ALLOC_PTR(req->ie);
		if (!req->ie)
			return false;
		atomic_fetch_add(&req->ie->ref, 1);
	}

	/* Create a new RPC ready for later use.  Take an initial reference
	 * to the RPC so that it is not cleaned up after a successful send.
	 *
	 * After calling send the lookup code will re-take the dropped
	 * reference which means that on all subsequent calls to reset()
	 * or release() the ref count will be two.
	 *
	 * This means that both descriptor creation and destruction are
	 * done off the critical path.
	 */
	rc = crt_req_create(req->fs_handle->proj.crt_ctx, NULL, req->opcode,
			    &req->request.rpc);
	if (rc || !req->request.rpc) {
		IOF_TRACE_ERROR(req, "Could not create request, rc = %d", rc);
		D_FREE(req->ie);
		return false;
	}
	crt_req_addref(req->request.rpc);

	return true;
}

/* Destroy a descriptor which could be either getattr or getfattr */
static void
entry_release(void *arg)
{
	struct entry_req *req = arg;

	crt_req_decref(req->request.rpc);
	crt_req_decref(req->request.rpc);
	D_FREE(req->ie);
}

static void
rb_page_init(void *arg, void *handle)
{
	struct iof_rb *rb = arg;

	rb->fs_handle = handle;
	rb->buf_size = 4096;
	rb->fbuf.count = 1;
	rb->fbuf.buf[0].fd = -1;
}

static void
rb_large_init(void *arg, void *handle)
{
	struct iof_rb *rb = arg;

	rb->fs_handle = handle;
	rb->buf_size = rb->fs_handle->max_read;
	rb->fbuf.count = 1;
	rb->fbuf.buf[0].fd = -1;
}

static bool
rb_reset(void *arg)
{
	struct iof_rb *rb = arg;
	int rc;

	rb->req = 0;

	if (rb->rpc) {
		crt_req_decref(rb->rpc);
		crt_req_decref(rb->rpc);
		rb->rpc = NULL;
	}

	if (rb->failure) {
		IOF_BULK_FREE(rb, lb);
		rb->failure = false;
	}

	if (!rb->lb.buf) {
		IOF_BULK_ALLOC(rb->fs_handle->proj.crt_ctx, rb, lb,
			       rb->buf_size, false);
		if (!rb->lb.buf)
			return false;
	}

	rc = crt_req_create(rb->fs_handle->proj.crt_ctx, NULL,
			    FS_TO_OP(fs_handle, readx), &rb->rpc);
	if (rc || !rb->rpc) {
		IOF_TRACE_ERROR(rb, "Could not create request, rc = %u", rc);
		IOF_BULK_FREE(rb, lb);
		return false;
	}
	crt_req_addref(rb->rpc);

	return true;
}

static void
rb_release(void *arg)
{
	struct iof_rb *rb = arg;

	IOF_BULK_FREE(rb, lb);

	crt_req_decref(rb->rpc);
	crt_req_decref(rb->rpc);
}

static void
wb_init(void *arg, void *handle)
{
	struct iof_wb *wb = arg;

	wb->fs_handle = handle;
}

static bool
wb_reset(void *arg)
{
	struct iof_wb *wb = arg;
	int rc;

	wb->req = 0;

	if (wb->rpc) {
		crt_req_decref(wb->rpc);
		crt_req_decref(wb->rpc);
		wb->rpc = NULL;
	}

	if (wb->failure) {
		IOF_BULK_FREE(wb, lb);
		wb->failure = false;
	}

	if (!wb->lb.buf) {
		IOF_BULK_ALLOC(wb->fs_handle->proj.crt_ctx, wb, lb,
			       wb->fs_handle->proj.max_write, true);
		if (!wb->lb.buf)
			return false;
	}

	rc = crt_req_create(wb->fs_handle->proj.crt_ctx, NULL,
			    FS_TO_OP(fs_handle, writex), &wb->rpc);
	if (rc || !wb->rpc) {
		IOF_TRACE_ERROR(wb, "Could not create request, rc = %u", rc);
		IOF_BULK_FREE(wb, lb);
		return false;
	}
	crt_req_addref(wb->rpc);

	return true;
}

static void
wb_release(void *arg)
{
	struct iof_wb *wb = arg;

	crt_req_decref(wb->rpc);
	crt_req_decref(wb->rpc);

	IOF_BULK_FREE(wb, lb);
}

static int
iof_check_complete(void *arg)
{
	struct iof_tracker *tracker = arg;

	return iof_tracker_test(tracker);
}

/* Call crt_progress() on a context until it returns timeout
 * or an error.
 *
 * Returns -DER_SUCCESS on timeout or passes through any other errors.
 */
static int
iof_progress_drain(struct iof_ctx *iof_ctx)
{
	int ctx_rc;

	do {
		ctx_rc = crt_progress(iof_ctx->crt_ctx, 1000000, NULL, NULL);

		if (ctx_rc != -DER_TIMEDOUT && ctx_rc != -DER_SUCCESS) {
			IOF_TRACE_WARNING(iof_ctx, "progress returned %d",
					  ctx_rc);
			return ctx_rc;
		}

	} while (ctx_rc != -DER_TIMEDOUT);
	return -DER_SUCCESS;
}

static void *
iof_thread(void *arg)
{
	struct iof_ctx	*iof_ctx = arg;
	int		ctx_rc;
	int		rc;

	iof_tracker_signal(&iof_ctx->thread_start_tracker);
	do {
		rc = crt_progress(iof_ctx->crt_ctx,
				  iof_ctx->poll_interval,
				  iof_ctx->callback_fn,
				  &iof_ctx->thread_stop_tracker);

		if (rc == -DER_TIMEDOUT) {
			rc = 0;
			sched_yield();
		}

		if (rc != 0)
			IOF_TRACE_ERROR(iof_ctx, "crt_progress failed rc: %d",
					rc);

	} while (!iof_tracker_test(&iof_ctx->thread_stop_tracker));

	if (rc != 0)
		IOF_TRACE_ERROR(iof_ctx, "crt_progress error on shutdown "
				"rc: %d", rc);

	do {
		/* If this context has a pool associated with it then reap
		 * any descriptors with it so there are no pending RPCs when
		 * we call context_destroy.
		 */
		if (iof_ctx->pool) {
			bool active;

			do {
				ctx_rc = iof_progress_drain(iof_ctx);

				active = iof_pool_reclaim(iof_ctx->pool);

				if (!active)
					break;

				IOF_TRACE_WARNING(iof_ctx,
						  "Active descriptors, waiting for one second");

			} while (active && ctx_rc == -DER_SUCCESS);
		} else {
			ctx_rc = iof_progress_drain(iof_ctx);
		}

		rc = crt_context_destroy(iof_ctx->crt_ctx, false);
		if (rc == -DER_BUSY)
			IOF_TRACE_WARNING(iof_ctx, "RPCs in flight, waiting");
		else if (rc != DER_SUCCESS)
			IOF_TRACE_ERROR(iof_ctx, "Could not destroy context %d",
					rc);
	} while (rc == -DER_BUSY && ctx_rc == -DER_SUCCESS);

	iof_tracker_signal(&iof_ctx->thread_shutdown_tracker);
	return (void *)(uintptr_t)rc;
}

/* Start a progress thread, return true on success */
static bool
iof_thread_start(struct iof_ctx *iof_ctx)
{
	int rc;

	iof_tracker_init(&iof_ctx->thread_start_tracker, 1);
	iof_tracker_init(&iof_ctx->thread_stop_tracker, 1);
	iof_tracker_init(&iof_ctx->thread_shutdown_tracker, 1);

	rc = pthread_create(&iof_ctx->thread, NULL,
			    iof_thread, iof_ctx);

	if (rc != 0) {
		IOF_TRACE_ERROR(iof_ctx, "Could not start progress thread");
		return false;
	}

	iof_tracker_wait(&iof_ctx->thread_start_tracker);
	return true;
}

/* Stop the progress thread, and destroy the cart context
 *
 * Returns the return code of crt_context_destroy()
 */
static int
iof_thread_stop(struct iof_ctx *iof_ctx)
{
	void *rtn;

	IOF_TRACE_INFO(iof_ctx, "Stopping CRT thread");
	iof_tracker_signal(&iof_ctx->thread_stop_tracker);
	iof_tracker_wait(&iof_ctx->thread_shutdown_tracker);
	pthread_join(iof_ctx->thread, &rtn);
	IOF_TRACE_INFO(iof_ctx,
		       "CRT thread stopped with %d",
		       (int)(uintptr_t)rtn);

	return (int)(uintptr_t)rtn;
}

static int iof_reg(void *arg, struct cnss_plugin_cb *cb, size_t cb_size)
{
	struct iof_state *iof_state = arg;
	struct iof_group_info *group;
	int ret;
	int num_attached = 0;
	int num_groups = 1;
	int i;

	iof_state->cb = cb;

	/* Hard code only the default group now */
	D_ALLOC_ARRAY(iof_state->groups, num_groups);
	if (iof_state->groups == NULL)
		return 1;

	/* Set this only after a successful allocation */
	iof_state->num_groups = num_groups;

	group = &iof_state->groups[0];
	group->grp_name = strdup(IOF_DEFAULT_SET);
	if (group->grp_name == NULL) {
		IOF_TRACE_ERROR(iof_state, "No memory available to configure "
				"IONSS");
		return 1;
	}

	D_INIT_LIST_HEAD(&iof_state->fs_list);

	cb->register_ctrl_constant_uint64(cb->plugin_dir, "ionss_count", 1);
	ret = cb->create_ctrl_subdir(cb->plugin_dir, "ionss",
				     &iof_state->ionss_dir);
	if (ret != 0) {
		IOF_TRACE_ERROR(iof_state, "Failed to create control dir for "
				"ionss info (rc = %d)\n", ret);
		return 1;
	}

	IOF_TRACE_UP(&iof_state->iof_ctx, iof_state, "iof_ctx");

	ret = crt_context_create(&iof_state->iof_ctx.crt_ctx);
	if (ret) {
		IOF_TRACE_ERROR(iof_state, "Context not created");
		return 1;
	}

	if (!iof_thread_start(&iof_state->iof_ctx)) {
		IOF_TRACE_ERROR(iof_state, "Failed to create progress thread");
		return 1;
	}

	/* Despite the hard coding above, now we can do attaches in a loop */
	for (i = 0; i < iof_state->num_groups; i++) {
		group = &iof_state->groups[i];
		group->grp.grp_id = i;

		if (!attach_group(iof_state, group)) {
			IOF_TRACE_ERROR(iof_state,
					"Failed to attach to service group '%s'",
					group->grp_name);
			continue;
		}
		group->crt_attached = true;
		num_attached++;
	}

	if (num_attached == 0) {
		IOF_TRACE_ERROR(iof_state, "No IONSS found");
		return 1;
	}

	cb->register_ctrl_constant_uint64(cb->plugin_dir, "ioctl_version",
					  IOF_IOCTL_VERSION);

	/*registrations*/
	ret = crt_register_eviction_cb(ioc_eviction_cb, iof_state);
	if (ret) {
		IOF_TRACE_ERROR(iof_state, "Eviction callback registration "
				"failed with ret: %d", ret);
		return ret;
	}

	ret = crt_rpc_register(QUERY_PSR_OP, 0, &QUERY_RPC_FMT);
	if (ret) {
		IOF_TRACE_ERROR(iof_state, "Query rpc registration failed with "
				"ret: %d", ret);
		return 1;
	}

	ret = crt_rpc_register(DETACH_OP, CRT_RPC_FEAT_NO_TIMEOUT, NULL);
	if (ret) {
		IOF_TRACE_ERROR(iof_state, "Detach registration failed with "
				"ret: %d", ret);
		return 1;
	}

	ret = iof_register(DEF_PROTO_CLASS(DEFAULT), NULL);
	if (ret) {
		IOF_TRACE_ERROR(iof_state, "RPC client registration failed "
				"with ret: %d", ret);
		return ret;
	}
	iof_state->cb_size = cb_size;

	return ret;
}

static uint64_t online_read_cb(void *arg)
{
	struct iof_projection_info *fs_handle = arg;

	return !FS_IS_OFFLINE(fs_handle);
}

static int online_write_cb(uint64_t value, void *arg)
{
	struct iof_projection_info *fs_handle = arg;

	if (value > 1)
		return EINVAL;

	if (value)
		fs_handle->offline_reason = 0;
	else
		fs_handle->offline_reason = EHOSTDOWN;

	return 0;
}

#define REGISTER_STAT(_STAT) cb->register_ctrl_variable(	\
		fs_handle->stats_dir,				\
		#_STAT,						\
		iof_uint_read,					\
		NULL, NULL,					\
		&fs_handle->stats->_STAT)
#define REGISTER_STAT64(_STAT) cb->register_ctrl_variable(	\
		fs_handle->stats_dir,				\
		#_STAT,						\
		iof_uint64_read,				\
		NULL, NULL,					\
		&fs_handle->stats->_STAT)

static bool
initialize_projection(struct iof_state *iof_state,
		      struct iof_group_info *group,
		      struct iof_fs_info *fs_info,
		      struct iof_psr_query *query,
		      int id)
{
	struct iof_projection_info	*fs_handle;
	struct cnss_plugin_cb		*cb;
	struct fuse_args		args = {0};
	bool				writeable = false;
	int				ret;

	struct iof_pool_reg pt = {.init = dh_init,
				  .reset = dh_reset,
				  .release = dh_release,
				  POOL_TYPE_INIT(iof_dir_handle, list)};

	struct iof_pool_reg fh = {.init = fh_init,
				  .reset = fh_reset,
				  .release = fh_release,
				  POOL_TYPE_INIT(iof_file_handle, list)};

	struct iof_pool_reg fgt = {.init = common_init,
				   .reset = gh_reset,
				   .release = common_release,
				   POOL_TYPE_INIT(common_req, list)};

	struct iof_pool_reg ct = {.init = common_init,
				  .reset = close_reset,
				  .release = common_release,
				  POOL_TYPE_INIT(common_req, list)};

	struct iof_pool_reg entry_t = {.reset = entry_reset,
				       .release = entry_release,
				       POOL_TYPE_INIT(entry_req, list)};

	struct iof_pool_reg rb_page = {.init = rb_page_init,
				       .reset = rb_reset,
				       .release = rb_release,
				       POOL_TYPE_INIT(iof_rb, list)};

	struct iof_pool_reg rb_large = {.init = rb_large_init,
					.reset = rb_reset,
					.release = rb_release,
					POOL_TYPE_INIT(iof_rb, list)};

	struct iof_pool_reg wb = {.init = wb_init,
				  .reset = wb_reset,
				  .release = wb_release,
				  POOL_TYPE_INIT(iof_wb, list)};

	cb = iof_state->cb;

	/* TODO: This is presumably wrong although it's not
	 * clear how best to handle it
	 */
	if (!iof_is_mode_supported(fs_info->flags))
		return false;

	if (fs_info->flags & IOF_WRITEABLE)
		writeable = true;

	D_ALLOC_PTR(fs_handle);
	if (!fs_handle)
		return false;

	IOF_TRACE_UP(fs_handle, iof_state, "iof_projection");

	ret = iof_pool_init(&fs_handle->pool, fs_handle);
	if (ret != 0)
		D_GOTO(err, 0);

	IOF_TRACE_UP(&fs_handle->pool, fs_handle, "iof_pool");

	fs_handle->iof_state = iof_state;
	fs_handle->flags = fs_info->flags;
	fs_handle->ctx.poll_interval = iof_state->iof_ctx.poll_interval;
	fs_handle->ctx.callback_fn = iof_state->iof_ctx.callback_fn;
	IOF_TRACE_INFO(fs_handle, "Filesystem mode: Private; "
			"Access: Read-%s | Fail Over: %s",
			fs_handle->flags & IOF_WRITEABLE
					 ? "Write" : "Only",
			fs_handle->flags & IOF_FAILOVER
					 ? "Enabled" : "Disabled");
	IOF_TRACE_INFO(fs_handle, "FUSE: %sthreaded | API => "
			"Write: ioc_ll_write%s, Read: fuse_reply_%s",
			fs_handle->flags & IOF_CNSS_MT
					 ? "Single " : "Multi-",
			fs_handle->flags & IOF_FUSE_WRITE_BUF ? "_buf" : "",
			fs_handle->flags & IOF_FUSE_READ_BUF ? "buf" : "data");

	ret = d_hash_table_create_inplace(D_HASH_FT_RWLOCK |
					  D_HASH_FT_EPHEMERAL,
					  fs_info->htable_size,
					  fs_handle, &hops,
					  &fs_handle->inode_ht);
	if (ret != 0)
		D_GOTO(err, 0);

	/* Keep a list of open file and directory handles
	 *
	 * Handles are added to these lists as the open call succeeds,
	 * and removed from the list when a release request is received,
	 * therefore this is a list of handles held locally by the
	 * kernel, not a list of handles the CNSS holds on the IONSS.
	 *
	 * Used during shutdown so that we can iterate over the list after
	 * terminating the FUSE thread to send close RPCs for any handles
	 * the server didn't close.
	 */
	D_INIT_LIST_HEAD(&fs_handle->opendir_list);
	pthread_mutex_init(&fs_handle->od_lock, NULL);
	D_INIT_LIST_HEAD(&fs_handle->openfile_list);
	pthread_mutex_init(&fs_handle->of_lock, NULL);

	fs_handle->max_read = fs_info->max_read;
	fs_handle->max_iov_read = fs_info->max_iov_read;
	fs_handle->proj.max_write = fs_info->max_write;
	fs_handle->proj.max_iov_write = fs_info->max_iov_write;
	fs_handle->readdir_size = fs_info->readdir_size;
	fs_handle->gah = fs_info->gah;

	strncpy(fs_handle->mnt_dir.name, fs_info->dir_name.name, NAME_MAX);

	IOF_TRACE_DEBUG(fs_handle,
			"Projected Mount %s", fs_handle->mnt_dir.name);

	IOF_TRACE_INFO(fs_handle, "Mountpoint for this projection: '%s'",
		       fs_handle->mnt_dir.name);

	fs_handle->fs_id = fs_info->id;
	fs_handle->proj.cli_fs_id = id;
	fs_handle->proj.progress_thread = 1;

	D_ALLOC_PTR(fs_handle->stats);
	if (!fs_handle->stats)
		D_GOTO(err, 0);

	snprintf(fs_handle->ctrl_dir.name,
		 NAME_MAX,
		 "%d",
		 fs_handle->proj.cli_fs_id);

	cb->create_ctrl_subdir(iof_state->projections_dir,
			       fs_handle->ctrl_dir.name,
			       &fs_handle->fs_dir);

	/* Register the mount point with the control
	 * filesystem
	 */
	D_ASPRINTF(fs_handle->mount_point,
		   "%s/%s",
		   cb->prefix,
		   fs_handle->mnt_dir.name);
	if (!fs_handle->mount_point)
		D_GOTO(err, 0);

	cb->register_ctrl_constant(fs_handle->fs_dir,
				   "mount_point",
				   fs_handle->mount_point);

	cb->register_ctrl_constant(fs_handle->fs_dir, "mode",
				   "private");

	cb->register_ctrl_constant_uint64(fs_handle->fs_dir,
					  "fs_id",
					  fs_handle->fs_id);

	cb->register_ctrl_constant_uint64(fs_handle->fs_dir,
					  "group_id", group->grp.grp_id);

	cb->register_ctrl_constant_uint64(fs_handle->fs_dir,
					  "max_read",
					  fs_handle->max_read);

	cb->register_ctrl_constant_uint64(fs_handle->fs_dir,
					  "max_iov_read",
					  fs_handle->max_iov_read);

	cb->register_ctrl_constant_uint64(fs_handle->fs_dir,
					  "max_write",
					  fs_handle->proj.max_write);

	cb->register_ctrl_constant_uint64(fs_handle->fs_dir,
					  "max_iov_write",
					  fs_handle->proj.max_iov_write);

	cb->register_ctrl_constant_uint64(fs_handle->fs_dir,
				  "readdir_size",
				  fs_handle->readdir_size);

	cb->register_ctrl_uint64_variable(fs_handle->fs_dir, "online",
					  online_read_cb,
					  online_write_cb,
					  fs_handle);

	cb->create_ctrl_subdir(fs_handle->fs_dir, "stats",
			       &fs_handle->stats_dir);

	REGISTER_STAT(opendir);
	REGISTER_STAT(readdir);
	REGISTER_STAT(closedir);
	REGISTER_STAT(getattr);
	REGISTER_STAT(readlink);
	REGISTER_STAT(statfs);
	REGISTER_STAT(ioctl);
	REGISTER_STAT(open);
	REGISTER_STAT(release);
	REGISTER_STAT(read);
	REGISTER_STAT(il_ioctl);
	REGISTER_STAT(lookup);
	REGISTER_STAT(forget);
	REGISTER_STAT64(read_bytes);

	if (writeable) {
		REGISTER_STAT(create);
		REGISTER_STAT(rmdir);
		REGISTER_STAT(mkdir);
		REGISTER_STAT(unlink);
		REGISTER_STAT(symlink);
		REGISTER_STAT(rename);
		REGISTER_STAT(write);
		REGISTER_STAT(fsync);
		REGISTER_STAT64(write_bytes);
	}

	IOF_TRACE_INFO(fs_handle, "Filesystem ID srv:%d cli:%d",
		       fs_handle->fs_id,
		       fs_handle->proj.cli_fs_id);

	fs_handle->proj.grp = &group->grp;
	fs_handle->proj.grp_id = group->grp.grp_id;

	ret = crt_context_create(&fs_handle->ctx.crt_ctx);
	if (ret) {
		IOF_TRACE_ERROR(fs_handle, "Could not create context");
		D_GOTO(err, 0);
	}

	fs_handle->proj.crt_ctx = fs_handle->ctx.crt_ctx;
	fs_handle->ctx.pool = &fs_handle->pool;

	/* TODO: Much better error checking is required here, not least
	 * terminating the thread if there are any failures in the rest of
	 * this function
	 */
	if (!iof_thread_start(&fs_handle->ctx)) {
		IOF_TRACE_ERROR(fs_handle, "Could not create thread");
		D_GOTO(err, 0);
	}

	args.argc = 4;
	if (!writeable)
		args.argc++;

	args.allocated = 1;
	D_ALLOC_ARRAY(args.argv, args.argc);
	if (!args.argv)
		D_GOTO(err, 0);

	D_STRNDUP(args.argv[0], "", 1);
	if (!args.argv[0])
		D_GOTO(err, 0);

	D_STRNDUP(args.argv[1], "-ofsname=IOF", 32);
	if (!args.argv[1])
		D_GOTO(err, 0);

	D_STRNDUP(args.argv[2], "-osubtype=pam", 32);
	if (!args.argv[2])
		D_GOTO(err, 0);

	D_ASPRINTF(args.argv[3], "-omax_read=%u", fs_handle->max_read);
	if (!args.argv[3])
		D_GOTO(err, 0);

	if (!writeable) {
		D_STRNDUP(args.argv[4], "-oro", 32);
		if (!args.argv[4])
			D_GOTO(err, 0);
	}

	fs_handle->fuse_ops = iof_get_fuse_ops(fs_handle->flags);
	if (!fs_handle->fuse_ops)
		D_GOTO(err, 0);

	/* Register the directory handle type
	 *
	 * This is done late on in the registration as the dh_int() and
	 * dh_reset() functions require access to fs_handle.
	 */
	fs_handle->dh_pool = iof_pool_register(&fs_handle->pool, &pt);
	if (!fs_handle->dh_pool)
		D_GOTO(err, 0);

	fs_handle->fgh_pool = iof_pool_register(&fs_handle->pool, &fgt);
	if (!fs_handle->fgh_pool)
		D_GOTO(err, 0);

	fs_handle->close_pool = iof_pool_register(&fs_handle->pool, &ct);
	if (!fs_handle->close_pool)
		D_GOTO(err, 0);

	entry_t.init = lookup_entry_init;
	fs_handle->lookup_pool = iof_pool_register(&fs_handle->pool, &entry_t);
	if (!fs_handle->lookup_pool)
		D_GOTO(err, 0);

	entry_t.init = mkdir_entry_init;
	fs_handle->mkdir_pool = iof_pool_register(&fs_handle->pool, &entry_t);
	if (!fs_handle->mkdir_pool)
		D_GOTO(err, 0);

	entry_t.init = symlink_entry_init;
	fs_handle->symlink_pool = iof_pool_register(&fs_handle->pool, &entry_t);
	if (!fs_handle->symlink_pool)
		D_GOTO(err, 0);

	fs_handle->fh_pool = iof_pool_register(&fs_handle->pool, &fh);
	if (!fs_handle->fh_pool)
		D_GOTO(err, 0);

	fs_handle->rb_pool_page = iof_pool_register(&fs_handle->pool, &rb_page);
	if (!fs_handle->rb_pool_page)
		D_GOTO(err, 0);

	fs_handle->rb_pool_large = iof_pool_register(&fs_handle->pool, &rb_large);
	if (!fs_handle->rb_pool_large)
		D_GOTO(err, 0);

	fs_handle->write_pool = iof_pool_register(&fs_handle->pool, &wb);
	if (!fs_handle->write_pool)
		D_GOTO(err, 0);

	if (!cb->register_fuse_fs(cb->handle,
				  NULL,
				  fs_handle->fuse_ops,
				  &args,
				  fs_handle->mnt_dir.name,
				 (fs_handle->flags & IOF_CNSS_MT) != 0,
				  fs_handle)) {
		IOF_TRACE_ERROR(fs_handle, "Unable to register FUSE fs");
		D_GOTO(err, 0);
	}

	IOF_TRACE_DEBUG(fs_handle, "Fuse mount installed at: '%s'",
			fs_handle->mnt_dir.name);

	d_list_add_tail(&fs_handle->link, &iof_state->fs_list);

	return true;
err:
	iof_pool_destroy(&fs_handle->pool);
	D_FREE(fs_handle);
	return false;
}

static bool
query_projections(struct iof_state *iof_state,
		  struct iof_group_info *group,
		  int *total, int *active)
{
	struct iof_fs_info *tmp;
	crt_rpc_t *query_rpc = NULL;
	struct iof_psr_query *query;
	int rc;
	int i;

	*total = *active = 0;

	/* Query the IONSS for initial information, including projection list
	 *
	 * Do this in a loop, until success, if there is a eviction then select
	 * a new endpoint and try again.  As this is the first RPC that IOF
	 * sends there is no cleanup to perform if this fails, as there is no
	 * server side-state or RPCs created at this point.
	 */
	do {
		rc = get_info(iof_state, group, &query_rpc);

		if (rc == -DER_OOG || rc == -DER_EVICTED) {
			d_rank_list_t *psr_list = NULL;

			rc = crt_lm_group_psr(group->grp.dest_grp, &psr_list);
			if (rc != -DER_SUCCESS || !psr_list)
				return false;
			if (psr_list->rl_nr < 1) {
				IOF_TRACE_WARNING(iof_state,
						  "No more ranks to try, giving up");
				d_rank_list_free(psr_list);
				return false;
			}

			IOF_TRACE_WARNING(iof_state,
					  "Changing IONNS rank from %d to %d",
					  group->grp.psr_ep.ep_rank,
					  psr_list->rl_ranks[0]);

			atomic_store_release(&group->grp.pri_srv_rank,
					     psr_list->rl_ranks[0]);
			group->grp.psr_ep.ep_rank = psr_list->rl_ranks[0];
			d_rank_list_free(psr_list);

		} else if (rc != -DER_SUCCESS) {
			IOF_TRACE_ERROR(iof_state,
					"Query operation failed: %d",
					rc);
			return false;
		}

	} while (rc != -DER_SUCCESS);

	if (!query_rpc) {
		IOF_TRACE_ERROR(iof_state, "Query operation failed");
		return false;
	}

	query = crt_reply_get(query_rpc);

	iof_state->iof_ctx.poll_interval = query->poll_interval;
	iof_state->iof_ctx.callback_fn = query->progress_callback ?
					 iof_check_complete : NULL;
	IOF_TRACE_INFO(iof_state, "Poll Interval: %u microseconds; "
				  "Progress Callback: %s", query->poll_interval,
		       query->progress_callback ? "Enabled" : "Disabled");

	if (query->count != query->query_list.iov_len / sizeof(struct iof_fs_info)) {
		IOF_TRACE_ERROR(iof_state,
				"Invalid response from IONSS %d",
				query->count);
		return false;
	}
	IOF_TRACE_DEBUG(iof_state, "Number of filesystems projected by %s: %d",
			group->grp_name, query->count);

	tmp = query->query_list.iov_buf;

	for (i = 0; i < query->count; i++) {
		if (!initialize_projection(iof_state, group, &tmp[i], query,
					   (*total)++)) {
			IOF_TRACE_ERROR(iof_state,
					"Could not initialize projection '%s' from %s",
					tmp[i].dir_name.name,
					group->grp_name);
			return false;
		}

		(*active)++;
	}

	crt_req_decref(query_rpc);

	return true;
}

static int iof_post_start(void *arg)
{
	struct cnss_plugin_cb	*cb;
	struct iof_state	*iof_state = arg;
	int			active_projections = 0;
	int			total_projections = 0;
	int grp_num;
	int ret;

	cb = iof_state->cb;

	ret = cb->create_ctrl_subdir(cb->plugin_dir, "projections",
				     &iof_state->projections_dir);
	if (ret != 0) {
		IOF_TRACE_ERROR(iof_state, "Failed to create control dir for "
				"PA mode (rc = %d)\n", ret);
		return 1;
	}

	for (grp_num = 0; grp_num < iof_state->num_groups; grp_num++) {
		struct iof_group_info *group = &iof_state->groups[grp_num];
		int active;

		if (!group->crt_attached)
			continue;

		if (!query_projections(iof_state, group, &total_projections,
				       &active)) {
			IOF_TRACE_ERROR(iof_state,
					"Couldn't mount projections from %s",
					group->grp_name);
			continue;
		}
		active_projections += active;

		group->iof_registered = true;
	}

	cb->register_ctrl_constant_uint64(cb->plugin_dir,
					  "projection_count",
					  total_projections);

	if (total_projections == 0) {
		IOF_TRACE_ERROR(iof_state, "No projections found");
		return 1;
	}

	if (active_projections == 0) {
		IOF_TRACE_ERROR(iof_state, "No projections found");
		return 1;
	}

	return 0;
}

static int
ino_flush(d_list_t *rlink, void *arg)
{
	struct fuse_session *session = arg;
	struct ioc_inode_entry *ie = container_of(rlink,
						  struct ioc_inode_entry,
						  list);
	int rc;

	if (ie->parent != 1)
		return 0;

	rc = fuse_lowlevel_notify_inval_entry(session,
					      ie->parent,
					      ie->name,
					      strlen(ie->name));
	if (rc != 0 && rc != -ENOENT)
		IOF_LOG_WARNING("%lu %lu '%s': %d",
				ie->parent, ie->ino, ie->name, rc);
	else
		IOF_LOG_INFO("%lu %lu '%s': %d",
			     ie->parent, ie->ino, ie->name, rc);

	return 0;
}

/* Called once per projection, before the FUSE filesystem has been torn down */
static void iof_flush_fuse(void *arg1, void *arg2)
{
	struct fuse_session *session = arg1;
	struct iof_projection_info *fs_handle = arg2;
	int rc;

	IOF_TRACE_INFO(fs_handle, "Flushing inode table");

	rc = d_hash_table_traverse(&fs_handle->inode_ht, ino_flush, session);

	IOF_TRACE_INFO(fs_handle, "Flush complete: %d", rc);
}

/* Called once per projection, after the FUSE filesystem has been torn down */
static void iof_deregister_fuse(void *arg)
{
	struct iof_projection_info *fs_handle = arg;
	d_list_t *rlink = NULL;
	struct iof_file_handle *fh, *fh2;
	struct iof_dir_handle *dh, *dh2;
	uint64_t refs = 0;
	int handles = 0;
	int rc;

	IOF_TRACE_INFO(fs_handle, "Draining inode table");
	do {
		struct ioc_inode_entry *ie;

		rlink = d_hash_rec_first(&fs_handle->inode_ht);
		IOF_TRACE_DEBUG(fs_handle, "rlink is %p", rlink);
		if (!rlink)
			break;

		ie = container_of(rlink, struct ioc_inode_entry, list);

		refs += ie->ref;
		ie->parent = 0;
		d_hash_rec_ndecref(&fs_handle->inode_ht, ie->ref, rlink);
		handles++;
	} while (rlink);

	IOF_TRACE_INFO(fs_handle, "dropped %lu refs on %u handles",
		       refs, handles);

	rc = d_hash_table_destroy_inplace(&fs_handle->inode_ht, false);
	if (rc)
		IOF_TRACE_WARNING(fs_handle, "Failed to close inode handles");

	/* This code does not need to hold the locks as the fuse progression
	 * thread is no longer running so no more calls to open()/opendir()
	 * or close()/releasedir() can race with this code.
	 */
	handles = 0;
	d_list_for_each_entry_safe(dh, dh2, &fs_handle->opendir_list, list) {
		IOF_TRACE_INFO(fs_handle, "Closing directory " GAH_PRINT_STR
			       " %p", GAH_PRINT_VAL(dh->gah), dh);
		ioc_int_releasedir(dh);
		handles++;
	}
	IOF_TRACE_INFO(fs_handle, "Closed %d directory handles", handles);

	handles = 0;
	d_list_for_each_entry_safe(fh, fh2, &fs_handle->openfile_list, list) {
		IOF_TRACE_INFO(fs_handle, "Closing file " GAH_PRINT_STR
			       " %p", GAH_PRINT_VAL(fh->common.gah), fh);
		ioc_int_release(fh);
		handles++;
	}
	IOF_TRACE_INFO(fs_handle, "Closed %d file handles", handles);

	/* Stop the progress thread for this projection and delete the context
	 */
	iof_thread_stop(&fs_handle->ctx);

	iof_pool_destroy(&fs_handle->pool);

	IOF_TRACE_DOWN(fs_handle);
	d_list_del(&fs_handle->link);

	D_FREE(fs_handle->mount_point);
	D_FREE(fs_handle->fuse_ops);

	pthread_mutex_destroy(&fs_handle->od_lock);

	D_FREE(fs_handle->stats);
	D_FREE(fs_handle);
}

static void
detach_cb(const struct crt_cb_info *cb_info)
{
	struct iof_tracker *tracker = cb_info->cci_arg;

	iof_tracker_signal(tracker);
}

static void iof_finish(void *arg)
{
	struct iof_state *iof_state = arg;
	struct iof_group_info *group;

	int ret;
	int i;
	struct iof_tracker tracker;

	iof_tracker_init(&tracker, iof_state->num_groups);

	for (i = 0; i < iof_state->num_groups; i++) {
		crt_rpc_t *rpc = NULL;

		group = &iof_state->groups[i];

		if (!group->iof_registered) {
			iof_tracker_signal(&tracker);
			continue;
		}

		/*send a detach RPC to IONSS*/
		ret = crt_req_create(iof_state->iof_ctx.crt_ctx,
				     &group->grp.psr_ep,
				     DETACH_OP, &rpc);
		if (ret || !rpc) {
			IOF_TRACE_ERROR(iof_state, "Could not create detach req"
					" ret = %d", ret);
			iof_tracker_signal(&tracker);
			continue;
		}

		ret = crt_req_send(rpc, detach_cb, &tracker);
		if (ret) {
			IOF_TRACE_ERROR(iof_state, "Detach RPC not sent");
			iof_tracker_signal(&tracker);
		}
	}

	/* If an error occurred above, there will be no need to call
	 * progress
	 */
	if (!iof_tracker_test(&tracker))
		iof_wait(iof_state->iof_ctx.crt_ctx, &tracker);

	for (i = 0; i < iof_state->num_groups; i++) {
		group = &iof_state->groups[i];

		D_FREE(group->grp_name);

		if (!group->crt_attached)
			continue;

		ret = crt_group_detach(group->grp.dest_grp);
		if (ret)
			IOF_TRACE_ERROR(iof_state, "crt_group_detach failed "
					"with ret = %d", ret);
	}

	/* Stop progress thread */
	iof_thread_stop(&iof_state->iof_ctx);

	IOF_TRACE_DOWN(&iof_state->iof_ctx);
	IOF_TRACE_DOWN(iof_state);
	D_FREE(iof_state->groups);
	D_FREE(iof_state);
}

struct cnss_plugin self = {.name		= "iof",
			   .version		= CNSS_PLUGIN_VERSION,
			   .require_service	= 0,
			   .start		= iof_reg,
			   .post_start		= iof_post_start,
			   .deregister_fuse	= iof_deregister_fuse,
			   .flush_fuse		= iof_flush_fuse,
			   .destroy_plugin_data	= iof_finish};

int iof_plugin_init(struct cnss_plugin **fns, size_t *size)
{
	struct iof_state *state;

	D_ALLOC_PTR(state);
	if (!state)
		return IOF_ERR_NOMEM;

	*size = sizeof(struct cnss_plugin);

	self.handle = state;
	*fns = &self;
	return 0;
}
