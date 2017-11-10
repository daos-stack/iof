/* Copyright (C) 2016-2017 Intel Corporation
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

#include <inttypes.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <libgen.h>

#include "iof_common.h"
#include "ioc.h"
#include "log.h"
#include "ios_gah.h"
#include "iof_ioctl.h"
#include "iof_pool.h"

struct query_cb_r {
	struct iof_tracker tracker;
	struct iof_psr_query **query;
	int err;
};

struct iof_projection_info *ioc_get_handle(void)
{
	struct fuse_context *context = fuse_get_context();

	return context->private_data;
}

/*
 * A common callback that is used by several of the I/O RPCs that only return
 * status, with no data or metadata, for example rmdir and truncate.
 *
 * out->err will always be a errno that can be passed back to FUSE.
 */
void
ioc_status_cb(const struct crt_cb_info *cb_info)
{
	struct status_cb_r *reply = cb_info->cci_arg;
	struct iof_status_out *out = crt_reply_get(cb_info->cci_rpc);

	if (cb_info->cci_rc != 0) {
		/*
		 * Error handling.  Return EIO on any error
		 */
		IOF_TRACE_INFO(reply, "Bad RPC reply %d", cb_info->cci_rc);
		reply->err = EIO;
		iof_tracker_signal(&reply->tracker);
		return;
	}

	if (out->err) {
		IOF_TRACE_DEBUG(reply, "reply indicates error %d", out->err);
		reply->err = EIO;
	}
	reply->rc = out->rc;
	iof_tracker_signal(&reply->tracker);
}

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

	IOF_FUSE_REPLY_ZERO(req);
	return;

out_err:
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
	rc = crt_lm_group_psr(group, &psr_list);
	IOF_TRACE_INFO(arg, "ListPtr: %p, List: %p, Ranks: %d",
		       psr_list, psr_list ? psr_list->rl_ranks : 0,
		       psr_list ? psr_list->rl_nr.num : -1);
	/* Sanity Check */
	if (rc || !psr_list || !psr_list->rl_ranks) {
		IOF_TRACE_ERROR(arg, "Invalid rank list, ret = %d", rc);
		if (!rc)
			rc = -EINVAL;
	}
	if (!rc && psr_list->rl_nr.num == 0) {
		/* No more ranks remaining. */
		IOF_TRACE_ERROR(arg, "No PSRs left to failover.");
		rc = -EHOSTDOWN;
	} else
		new_psr = psr_list->rl_ranks[0];
	if (psr_list != NULL)
		d_rank_list_free(psr_list);

	for (i = 0; i < iof_state->num_groups; i++) {
		g = iof_state->groups + i;
		if (!strncmp(group->cg_grpid,
			    g->grp.dest_grp->cg_grpid,
			    CRT_GROUP_ID_MAX_LEN))
			break;
	}

	if (g == NULL) {
		IOF_TRACE_WARNING(arg, "No group found: %s",
				  group->cg_grpid);
		return;
	} else if (rc) {
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
		if (fs_handle->proj.grp != &g->grp)
			continue;

		if (!g->grp.enabled || !IOF_HAS_FAILOVER(fs_handle->flags)) {
			IOF_TRACE_WARNING(fs_handle,
					  "Marking projection %d offline: %s",
					  fs_handle->fs_id,
					  fs_handle->mount_point);
			if (!g->grp.enabled)
				fs_handle->offline_reason = -rc;
			else
				fs_handle->offline_reason = EHOSTDOWN;
			continue;
		}
		/* TODO: Migrate Open Handles */
	}
}

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
	fuse_req_t f_req = request->req;

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
	if (f_req == NULL)
		iof_tracker_signal(&request->tracker);
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
	rc = crt_req_addref(request->rpc);
	if (rc)
		D_GOTO(err, rc);
	rc = crt_req_set_endpoint(request->rpc, &request->ep);
	if (rc)
		D_GOTO(err, rc);
	IOF_TRACE_INFO(request, "Sending RPC to PSR Rank %d",
		      request->rpc->cr_ep.ep_rank);
	rc = crt_req_send(request->rpc, generic_cb, request);
	if (rc)
		D_GOTO(err, rc);
	if (request->cb->on_send)
		request->cb->on_send(request);
	return 0;
err:
	IOF_TRACE_ERROR(request, "Could not send rpc, rc = %d", rc);
	return -EIO;
}

static void
query_cb(const struct crt_cb_info *cb_info)
{
	struct query_cb_r *reply = cb_info->cci_arg;
	struct iof_psr_query *query = crt_reply_get(cb_info->cci_rpc);
	int ret;

	if (cb_info->cci_rc != 0) {
		/*
		 * Error handling.  On timeout return EAGAIN, all other errors
		 * return EIO.
		 *
		 * TODO: Handle target eviction here
		 */
		IOF_TRACE_INFO(reply, "Bad RPC reply %d", cb_info->cci_rc);
		reply->err = cb_info->cci_rc;
		iof_tracker_signal(&reply->tracker);
		return;
	}

	ret = crt_req_addref(cb_info->cci_rpc);
	if (ret) {
		IOF_TRACE_ERROR(reply, "could not take reference on query RPC, "
				"ret = %d", ret);
		iof_tracker_signal(&reply->tracker);
		return;
	}

	*reply->query = query;

	iof_tracker_signal(&reply->tracker);
}

/*Send RPC to PSR to get information about projected filesystems*/
static int ioc_get_projection_info(struct iof_state *iof_state,
				   struct iof_group_info *group,
				   struct iof_psr_query **query,
				   crt_rpc_t **query_rpc)
{
	int ret;
	struct query_cb_r reply = {0};

	iof_tracker_init(&reply.tracker, 1);

	reply.query = query;

	ret = crt_req_create(iof_state->crt_ctx, &group->grp.psr_ep,
			     QUERY_PSR_OP, query_rpc);
	if (ret || (*query_rpc == NULL)) {
		IOF_TRACE_ERROR(iof_state, "failed to create query rpc request,"
				" ret = %d", ret);
		return ret;
	}
	IOF_TRACE_LINK(*query_rpc, iof_state, "query_rpc");

	ret = crt_req_send(*query_rpc, query_cb, &reply);
	if (ret) {
		IOF_TRACE_ERROR(iof_state, "Could not send query RPC, ret = %d",
				ret);
		return ret;
	}

	/*make on-demand progress*/
	iof_wait(iof_state->crt_ctx, &reply.tracker);

	if (reply.err)
		return reply.err;

	return ret;
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

#define BUFSIZE 64
static int attach_group(struct iof_state *iof_state,
			struct iof_group_info *group, int id)
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
		return ret;
	}

	ret = iof_lm_attach(group->grp.dest_grp, NULL);
	if (ret != 0) {
		IOF_TRACE_ERROR(iof_state,
				"Could not initialize failover, ret = %d", ret);
		return ret;
	}

	ret = crt_group_config_save(group->grp.dest_grp, true);
	if (ret) {
		IOF_TRACE_ERROR(iof_state, "crt_group_config_save failed for "
				"ionss with ret = %d", ret);
		return ret;
	}

	/*initialize destination endpoint*/
	group->grp.psr_ep.ep_grp = group->grp.dest_grp;
	ret = crt_lm_group_psr(group->grp.dest_grp, &psr_list);
	IOF_TRACE_INFO(group, "ListPtr: %p, List: %p, Ranks: %d",
		       psr_list, psr_list ? psr_list->rl_ranks : 0,
		       psr_list ? psr_list->rl_nr.num : 0);
	if (ret || !psr_list || !psr_list->rl_ranks || !psr_list->rl_nr.num) {
		IOF_TRACE_ERROR(group, "Unable to access "
				"PSR list, ret = %d", ret);
		return ret;
	}
	/* First element in the list is the PSR */
	atomic_store_release(&group->grp.pri_srv_rank, psr_list->rl_ranks[0]);
	group->grp.psr_ep.ep_rank = psr_list->rl_ranks[0];
	group->grp.psr_ep.ep_tag = 0;
	group->grp.grp_id = id;
	d_rank_list_free(psr_list);
	IOF_TRACE_INFO(group, "Primary Service Rank: %d",
		       atomic_load_consume(&group->grp.pri_srv_rank));

	sprintf(buf, "%d", id);

	ret = cb->create_ctrl_subdir(iof_state->ionss_dir, buf,
				     &ionss_dir);
	if (ret != 0) {
		IOF_TRACE_ERROR(iof_state, "Failed to create control dir for "
				"ionss info (rc = %d)\n", ret);
		return IOF_ERR_CTRL_FS;
	}
	cb->register_ctrl_constant_uint64(ionss_dir, "psr_rank",
					  group->grp.psr_ep.ep_rank);
	cb->register_ctrl_constant_uint64(ionss_dir, "psr_tag",
					  group->grp.psr_ep.ep_tag);
	/* Fix this when we actually have multiple IONSS apps */
	cb->register_ctrl_constant(ionss_dir, "name", group->grp_name);

	group->grp.enabled = true;

	return 0;
}

static bool ih_key_cmp(struct d_chash_table *htable, d_list_t *rlink,
		       const void *key, unsigned int ksize)
{
	const struct ioc_inode_entry *ie;
	const fuse_ino_t *ino = key;

	ie = container_of(rlink, struct ioc_inode_entry, list);

	return *ino == ie->ino;
}

static void ih_addref(struct d_chash_table *htable, d_list_t *rlink)
{
	struct ioc_inode_entry *ie;
	int oldref;

	ie = container_of(rlink, struct ioc_inode_entry, list);
	oldref = atomic_fetch_add(&ie->ref, 1);
	IOF_TRACE_DEBUG(ie, "addref to %u", oldref + 1);
}

static bool ih_decref(struct d_chash_table *htable, d_list_t *rlink)
{
	struct ioc_inode_entry *ie;
	int oldref;

	ie = container_of(rlink, struct ioc_inode_entry, list);
	oldref = atomic_fetch_sub(&ie->ref, 1);
	IOF_TRACE_DEBUG(ie, "decref to %u", oldref - 1);
	return oldref == 1;
}

static void ih_free(struct d_chash_table *htable, d_list_t *rlink)
{
	struct iof_projection_info *fs_handle = htable->ht_priv;
	struct ioc_inode_entry *ie;

	ie = container_of(rlink, struct ioc_inode_entry, list);

	IOF_TRACE_DEBUG(ie);
	ie_close(fs_handle, ie);
	D_FREE(ie);
}

d_chash_table_ops_t hops = {.hop_key_cmp = ih_key_cmp,
			    .hop_rec_addref = ih_addref,
			    .hop_rec_decref = ih_decref,
			    .hop_rec_free = ih_free,
};

int dh_init(void *arg, void *handle)
{
	struct iof_dir_handle *dh = arg;

	dh->fs_handle = handle;

	return 0;
}

int dh_reset(void *arg)
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
		return -1;
	IOF_TRACE_LINK(dh->open_req.rpc, dh, "opendir_rpc");

	rc = crt_req_create(dh->fs_handle->proj.crt_ctx, NULL,
			    FS_TO_OP(dh->fs_handle, closedir),
			    &dh->close_req.rpc);
	if (rc || !dh->close_req.rpc) {
		crt_req_decref(dh->open_req.rpc);
		return -1;
	}
	IOF_TRACE_LINK(dh->close_req.rpc, dh, "closedir_rpc");
	return 0;
}

void dh_release(void *arg)
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
static int
fh_init(void *arg, void *handle)
{
	struct iof_file_handle *fh = arg;

	fh->fs_handle = handle;
	return 0;
}

static int
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
			return -1;
		atomic_fetch_add(&fh->ie->ref, 1);
	}

	rc = crt_req_create(fh->fs_handle->proj.crt_ctx, &fh->common.ep,
			    FS_TO_OP(fh->fs_handle, open), &fh->open_rpc);
	if (rc || !fh->open_rpc) {
		D_FREE(fh->ie);
		return -1;
	}
	IOF_TRACE_LINK(fh->open_rpc, fh, "open_rpc");

	rc = crt_req_create(fh->fs_handle->proj.crt_ctx, &fh->common.ep,
			    FS_TO_OP(fh->fs_handle, create), &fh->creat_rpc);
	if (rc || !fh->creat_rpc) {
		D_FREE(fh->ie);
		crt_req_decref(fh->open_rpc);
		return -1;
	}
	IOF_TRACE_LINK(fh->creat_rpc, fh, "creat_rpc");

	rc = crt_req_create(fh->fs_handle->proj.crt_ctx, &fh->common.ep,
			    FS_TO_OP(fh->fs_handle, close), &fh->release_rpc);
	if (rc || !fh->release_rpc) {
		D_FREE(fh->ie);
		crt_req_decref(fh->open_rpc);
		crt_req_decref(fh->creat_rpc);
		return -1;
	}
	IOF_TRACE_LINK(fh->release_rpc, fh, "release_rpc");

	crt_req_addref(fh->open_rpc);
	crt_req_addref(fh->creat_rpc);
	crt_req_addref(fh->release_rpc);
	return 0;
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

static int
gh_init(void *arg, void *handle)
{
	struct getattr_req *req = arg;

	req->fs_handle = handle;
	return 0;
}

/* Reset, and prepare for use a getattr descriptor */
static int
gh_reset(void *arg)
{
	struct getattr_req *req = arg;
	struct iof_gah_string_in *in;
	int rc;

	/* If this descriptor has previously been used the destroy the
	 * existing RPC
	 */
	if (req->request.rpc) {
		crt_req_decref(req->request.rpc);
		crt_req_decref(req->request.rpc);
		req->request.rpc = NULL;
	}

	/* Create a new RPC ready for later use.  Take an initial reference
	 * to the RPC so that it is not cleaned up after a successful send.
	 *
	 * After calling send the getattr code will re-take the dropped
	 * reference which means that on all subsequent calls to reset()
	 * or release() the ref count will be two.
	 *
	 * This means that both descriptor creation and destruction are
	 * done off the critical path.
	 */
	rc = crt_req_create(req->fs_handle->proj.crt_ctx, NULL,
			    FS_TO_OP(req->fs_handle, getattr),
			    &req->request.rpc);
	if (rc || !req->request.rpc) {
		IOF_TRACE_ERROR(req, "Could not create request, rc = %u", rc);
		return -1;
	}
	crt_req_addref(req->request.rpc);
	in = crt_req_get(req->request.rpc);
	in->gah = req->fs_handle->gah;

	return 0;
}

/* Reset and prepare for use a getfattr descriptor */
static int
fgh_reset(void *arg)
{
	struct getattr_req *req = arg;
	int rc;

	if (req->request.rpc) {
		crt_req_decref(req->request.rpc);
		crt_req_decref(req->request.rpc);
		req->request.rpc = NULL;
	}

	rc = crt_req_create(req->fs_handle->proj.crt_ctx, NULL,
			    FS_TO_OP(req->fs_handle, getattr_gah),
			    &req->request.rpc);
	if (rc || !req->request.rpc) {
		IOF_TRACE_ERROR(req, "Could not create request, rc = %u", rc);
		return -1;
	}
	crt_req_addref(req->request.rpc);

	return 0;
}

/* Destroy a descriptor which could be either getattr or getfattr */
static void
gh_release(void *arg)
{
	struct getattr_req *req = arg;

	crt_req_decref(req->request.rpc);
	crt_req_decref(req->request.rpc);
}

#define entry_init(type)					\
	static int type##_entry_init(void *arg, void *handle)	\
	{							\
		struct entry_req *req = arg;			\
								\
		req->fs_handle = handle;			\
		req->opcode = FS_TO_OP(dh->fs_handle, type);	\
		return 0;					\
	}
entry_init(lookup);
entry_init(mkdir_ll);
entry_init(symlink_ll);

static int
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

	if (!req->ie) {
		D_ALLOC_PTR(req->ie);
		if (!req->ie)
			return -1;
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
		return -1;
	}
	crt_req_addref(req->request.rpc);

	return 0;
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

static int
rb_page_reset(void *arg)
{
	struct iof_rb *rb = arg;

	if (rb->buf)
		return 0;

	D_ALLOC_PTR(rb->buf);
	if (!rb->buf)
		return -1;

	D_ALLOC(rb->buf->buf[0].mem, 4096);
	if (!rb->buf->buf[0].mem) {
		D_FREE(rb->buf);
		return -1;
	}

	rb->buf->count = 1;
	rb->buf->buf[0].fd = -1;

	return 0;
}

static int
rb_large_init(void *arg, void *handle)
{
	struct iof_rb *rb = arg;

	rb->fs_handle = handle;

	return 0;
}

static int
rb_large_reset(void *arg)
{
	struct iof_rb *rb = arg;

	if (rb->buf)
		return 0;

	/* TODO: This should use MMAP */
	D_ALLOC_PTR(rb->buf);
	if (!rb->buf)
		return -1;

	D_ALLOC(rb->buf->buf[0].mem, rb->fs_handle->max_read);
	if (!rb->buf->buf[0].mem) {
		D_FREE(rb->buf);
		return -1;
	}

	rb->buf->count = 1;
	rb->buf->buf[0].fd = -1;

	return 0;
}

static void
rb_release(void *arg)
{
	struct iof_rb *rb = arg;

	D_FREE(rb->buf->buf[0].mem);
	D_FREE(rb->buf);
}

static int iof_thread_start(struct iof_state *iof_state);
static void iof_thread_stop(struct iof_state *iof_state);

static int iof_reg(void *arg, struct cnss_plugin_cb *cb, size_t cb_size)
{
	struct iof_state *iof_state = arg;
	struct iof_group_info *group;
	char *prefix;
	int ret;
	DIR *prefix_dir;
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

	ret = crt_context_create(NULL, &iof_state->crt_ctx);
	if (ret) {
		IOF_TRACE_ERROR(iof_state, "Context not created");
		return 1;
	}

	iof_tracker_init(&iof_state->thread_start_tracker, 1);
	iof_tracker_init(&iof_state->thread_stop_tracker, 1);
	iof_tracker_init(&iof_state->thread_shutdown_tracker, 1);
	ret = iof_thread_start(iof_state);
	if (ret != 0) {
		IOF_TRACE_ERROR(iof_state, "Failed to create progress thread");
		return 1;
	}

	/* Despite the hard coding above, now we can do attaches in a loop */
	for (i = 0; i < iof_state->num_groups; i++) {
		group = &iof_state->groups[i];

		ret = attach_group(iof_state, group, i);
		if (ret != 0) {
			IOF_TRACE_ERROR(iof_state, "Failed to attach to service"
					" group %s (ret = %d)",
					group->grp_name, ret);
			continue;
		}
		group->attached = true;
		num_attached++;
	}

	if (num_attached == 0) {
		IOF_TRACE_ERROR(iof_state, "No IONSS found");
		return 1;
	}

	cb->register_ctrl_constant_uint64(cb->plugin_dir, "ioctl_version",
					  IOF_IOCTL_VERSION);

	prefix = getenv("CNSS_PREFIX");
	iof_state->cnss_prefix = realpath(prefix, NULL);
	prefix_dir = opendir(iof_state->cnss_prefix);
	if (prefix_dir)
		closedir(prefix_dir);
	else {
		if (mkdir(iof_state->cnss_prefix, 0755)) {
			IOF_TRACE_ERROR(iof_state, "Could not create "
					"cnss_prefix");
			return 1;
		}
	}

	/*registrations*/
	ret = crt_register_eviction_cb(ioc_eviction_cb, iof_state);
	if (ret) {
		IOF_TRACE_ERROR(iof_state, "Eviction callback registration "
				"failed with ret: %d", ret);
		return ret;
	}

	ret = crt_rpc_register(QUERY_PSR_OP, CRT_RPC_FEAT_NO_TIMEOUT,
			       &QUERY_RPC_FMT);
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

static int
iof_check_complete(void *arg)
{
	struct iof_tracker *tracker = arg;

	return iof_tracker_test(tracker);
}

static void *
iof_thread(void *arg)
{
	struct iof_state *iof_state = arg;
	int			rc;

	iof_tracker_signal(&iof_state->thread_start_tracker);
	do {
		rc = crt_progress(iof_state->crt_ctx, 1, iof_check_complete,
				  &iof_state->thread_stop_tracker);

		if (rc == -DER_TIMEDOUT) {
			rc = 0;
			sched_yield();
		}

		if (rc != 0)
			IOF_TRACE_ERROR(iof_state, "crt_progress failed rc: %d",
					rc);

	} while (!iof_tracker_test(&iof_state->thread_stop_tracker));

	if (rc != 0)
		IOF_TRACE_ERROR(iof_state, "crt_progress error on shutdown "
				"rc: %d", rc);

	iof_tracker_signal(&iof_state->thread_shutdown_tracker);
	return NULL;
}

static int
iof_thread_start(struct iof_state *iof_state)
{
	int rc;

	rc = pthread_create(&iof_state->thread, NULL,
			    iof_thread, iof_state);

	if (rc != 0) {
		IOF_TRACE_ERROR(iof_state, "Could not start progress thread");
		return 1;
	}

	iof_tracker_wait(&iof_state->thread_start_tracker);
	return 0;
}

static void
iof_thread_stop(struct iof_state *iof_state)
{
	IOF_TRACE_INFO(iof_state, "Stopping CRT thread");
	iof_tracker_signal(&iof_state->thread_stop_tracker);
	iof_tracker_wait(&iof_state->thread_shutdown_tracker);
	pthread_join(iof_state->thread, 0);
	IOF_TRACE_INFO(iof_state, "Stopped CRT thread");
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

static int initialize_projection(struct iof_state *iof_state,
				 struct iof_group_info *group,
				 struct iof_fs_info *fs_info,
				 struct iof_psr_query *query,
				 int id)
{
	struct iof_projection_info	*fs_handle;
	struct cnss_plugin_cb		*cb;
	struct fuse_args		args = {0};
	bool				writeable = false;
	char				*base_name;
	int				ret;

	cb = iof_state->cb;

	/* TODO: This is presumably wrong although it's not
	 * clear how best to handle it
	 */
	if (!iof_is_mode_supported(fs_info->flags))
		return IOF_NOT_SUPP;

	if (fs_info->flags & IOF_WRITEABLE)
		writeable = true;

	D_ALLOC_PTR(fs_handle);
	if (!fs_handle)
		return IOF_ERR_NOMEM;
	IOF_TRACE_UP(fs_handle, iof_state, "iof_projection");

	ret = iof_pool_init(&fs_handle->pool);
	if (ret != 0) {
		D_FREE(fs_handle);
		return IOF_ERR_NOMEM;
	}
	IOF_TRACE_UP(&fs_handle->pool, fs_handle, "iof_pool");

	fs_handle->iof_state = iof_state;
	fs_handle->flags = fs_info->flags;
	IOF_TRACE_INFO(fs_handle, "Filesystem mode: Private");
	if (IOF_HAS_FAILOVER(fs_handle->flags))
		IOF_TRACE_INFO(fs_handle, "Fail Over Enabled");

	ret = d_chash_table_create_inplace(D_HASH_FT_RWLOCK |
					   D_HASH_FT_EPHEMERAL,
					   4, fs_handle, &hops,
					   &fs_handle->inode_ht);
	if (ret != 0) {
		D_FREE(fs_handle);
		return IOF_ERR_NOMEM;
	}

	fs_handle->max_read = query->max_read;
	fs_handle->max_iov_read = query->max_iov_read;
	fs_handle->proj.max_write = query->max_write;
	fs_handle->proj.max_iov_write = query->max_iov_write;
	fs_handle->readdir_size = query->readdir_size;
	fs_handle->gah = fs_info->gah;

	base_name = basename(fs_info->mnt);

	ret = asprintf(&fs_handle->mount_point, "%s/%s",
		       iof_state->cnss_prefix, base_name);
	if (ret == -1)
		return IOF_ERR_NOMEM;

	IOF_TRACE_DEBUG(fs_handle, "Projected Mount %s", base_name);

	IOF_TRACE_INFO(fs_handle, "Mountpoint for this projection: %s",
		       fs_handle->mount_point);

	fs_handle->fs_id = fs_info->id;
	fs_handle->proj.cli_fs_id = id;
	fs_handle->proj.progress_thread = 1;

	D_ALLOC_PTR(fs_handle->stats);
	if (!fs_handle->stats)
		return 1;

	ret = asprintf(&fs_handle->base_dir, "%d",
		       fs_handle->proj.cli_fs_id);
	if (ret == -1)
		return IOF_ERR_NOMEM;

	cb->create_ctrl_subdir(iof_state->projections_dir,
			       fs_handle->base_dir,
			       &fs_handle->fs_dir);

	/* Register the mount point with the control
	 * filesystem
	 */
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
	REGISTER_STAT(getfattr);
	REGISTER_STAT(il_ioctl);
	REGISTER_STAT(lookup);
	REGISTER_STAT(forget);
	REGISTER_STAT64(read_bytes);

	if (writeable) {
		REGISTER_STAT(chmod);
		REGISTER_STAT(create);
		REGISTER_STAT(rmdir);
		REGISTER_STAT(mkdir);
		REGISTER_STAT(unlink);
		REGISTER_STAT(symlink);
		REGISTER_STAT(rename);
		REGISTER_STAT(truncate);
		REGISTER_STAT(utimens);
		REGISTER_STAT(write);
		REGISTER_STAT(ftruncate);
		REGISTER_STAT(fchmod);
		REGISTER_STAT(futimens);
		REGISTER_STAT(fsync);
		REGISTER_STAT64(write_bytes);
	}

	IOF_TRACE_INFO(fs_handle, "Filesystem ID srv:%d cli:%d",
		       fs_handle->fs_id,
		       fs_handle->proj.cli_fs_id);

	fs_handle->proj.grp = &group->grp;
	fs_handle->proj.grp_id = group->grp.grp_id;
	fs_handle->proj.crt_ctx = iof_state->crt_ctx;

	args.argc = 4;
	if (!writeable)
		args.argc++;

	args.allocated = 1;
	args.argv = calloc(args.argc, sizeof(char *));
	if (!args.argv)
		return IOF_ERR_NOMEM;

	ret = asprintf(&args.argv[0], "%s", "");
	if (ret == -1)
		return IOF_ERR_NOMEM;

	ret = asprintf(&args.argv[1], "-ofsname=IOF");
	if (ret == -1)
		return IOF_ERR_NOMEM;

	ret = asprintf(&args.argv[2], "-osubtype=pam");
	if (ret == -1)
		return IOF_ERR_NOMEM;

	ret = asprintf(&args.argv[3], "-omax_read=%u", fs_handle->max_read);
	if (ret == -1)
		return IOF_ERR_NOMEM;

	if (!writeable) {
		ret = asprintf(&args.argv[4], "-oro");
		if (ret == -1)
			return IOF_ERR_NOMEM;
	}

	if (writeable) {
		fs_handle->fuse_ops = iof_get_fuse_ops(fs_handle->flags);
		if (!fs_handle->fuse_ops)
			return IOF_ERR_NOMEM;
	} else {
		fs_handle->fuse_ll_ops = iof_get_fuse_ll_ops(writeable);
		if (!fs_handle->fuse_ll_ops)
			return IOF_ERR_NOMEM;
	}

	ret = cb->register_fuse_fs(cb->handle,
				   fs_handle->fuse_ops,
				   fs_handle->fuse_ll_ops,
				   &args,
				   fs_handle->mount_point,
				  (fs_handle->flags & IOF_CNSS_MT) != 0,
				   fs_handle);
	if (ret) {
		IOF_TRACE_ERROR(fs_handle, "Unable to register FUSE fs");
		D_FREE(fs_handle);
		return 1;
	}

	{
		/* Register the directory handle type
		 *
		 * This is done late on in the registraction as the dh_int()
		 * and dh_reset() functions require access to fs_handle.
		 */
		struct iof_pool_reg pt = { .handle = fs_handle,
					   .init = dh_init,
					   .reset = dh_reset,
					   .release = dh_release,
					   POOL_TYPE_INIT(iof_dir_handle, list)};

		struct iof_pool_reg fh = { .handle = fs_handle,
					   .init = fh_init,
					   .reset = fh_reset,
					   .release = fh_release,
					   POOL_TYPE_INIT(iof_file_handle, list)};

		struct iof_pool_reg gt = { .handle = fs_handle,
					   .init = gh_init,
					   .reset = gh_reset,
					   .release = gh_release,
					   POOL_TYPE_INIT(getattr_req, list)};

		struct iof_pool_reg fgt = { .handle = fs_handle,
					    .init = gh_init,
					    .reset = fgh_reset,
					    .release = gh_release,
					    POOL_TYPE_INIT(getattr_req, list)};

		struct iof_pool_reg entry_t = { .handle = fs_handle,
						.reset = entry_reset,
						.release = entry_release,
						POOL_TYPE_INIT(entry_req, list)};

		struct iof_pool_reg rb_page = { .handle = fs_handle,
						.reset = rb_page_reset,
						.release = rb_release,
						POOL_TYPE_INIT(iof_rb, list)};

		struct iof_pool_reg rb_large = { .handle = fs_handle,
						 .init = rb_large_init,
						 .reset = rb_large_reset,
						 .release = rb_release,
						 POOL_TYPE_INIT(iof_rb, list)};

		fs_handle->dh_pool = iof_pool_register(&fs_handle->pool, &pt);
		if (!fs_handle->dh_pool)
			return IOF_ERR_NOMEM;

		fs_handle->gh_pool = iof_pool_register(&fs_handle->pool, &gt);
		if (!fs_handle->gh_pool)
			return IOF_ERR_NOMEM;

		fs_handle->fgh_pool = iof_pool_register(&fs_handle->pool, &fgt);
		if (!fs_handle->fgh_pool)
			return IOF_ERR_NOMEM;

		entry_t.init = lookup_entry_init;
		fs_handle->lookup_pool = iof_pool_register(&fs_handle->pool,
							   &entry_t);
		if (!fs_handle->lookup_pool)
			return IOF_ERR_NOMEM;

		entry_t.init = mkdir_ll_entry_init;
		fs_handle->mkdir_pool = iof_pool_register(&fs_handle->pool,
							  &entry_t);
		if (!fs_handle->mkdir_pool)
			return IOF_ERR_NOMEM;

		entry_t.init = symlink_ll_entry_init;
		fs_handle->symlink_pool = iof_pool_register(&fs_handle->pool,
							    &entry_t);
		if (!fs_handle->symlink_pool)
			return IOF_ERR_NOMEM;

		fs_handle->fh_pool = iof_pool_register(&fs_handle->pool, &fh);
		if (!fs_handle->fh_pool)
			return IOF_ERR_NOMEM;

		fs_handle->rb_pool_page = iof_pool_register(&fs_handle->pool,
							    &rb_page);
		if (!fs_handle->rb_pool_page)
			return IOF_ERR_NOMEM;

		fs_handle->rb_pool_large = iof_pool_register(&fs_handle->pool,
							     &rb_large);
		if (!fs_handle->rb_pool_large)
			return IOF_ERR_NOMEM;
	}

	IOF_TRACE_DEBUG(fs_handle, "Fuse mount installed at: %s",
			fs_handle->mount_point);

	d_list_add_tail(&fs_handle->link, &iof_state->fs_list);

	return 0;
}

static int query_projections(struct iof_state *iof_state,
			     struct iof_group_info *group,
			     int *total, int *active)
{
	struct iof_fs_info *tmp;
	crt_rpc_t *query_rpc = NULL;
	struct iof_psr_query *query = NULL;
	int fs_num;
	int i;
	int ret;

	*total = *active = 0;

	/*Query PSR*/
	ret = ioc_get_projection_info(iof_state, group, &query,
				      &query_rpc);
	if (ret || (query == NULL)) {
		IOF_TRACE_ERROR(iof_state, "Query operation failed");
		return IOF_ERR_PROJECTION;
	}
	IOF_TRACE_LINK(query_rpc, iof_state, "query_rpc");

	/*calculate number of projections*/
	fs_num = (query->query_list.iov_len)/sizeof(struct iof_fs_info);
	IOF_TRACE_DEBUG(iof_state, "Number of filesystems projected by %s: %d",
			group->grp_name, fs_num);
	tmp = (struct iof_fs_info *) query->query_list.iov_buf;

	for (i = 0; i < fs_num; i++) {
		ret = initialize_projection(iof_state, group, &tmp[i], query,
					    (*total)++);

		if (ret != 0) {
			IOF_TRACE_ERROR(iof_state, "Could not initialize "
					"projection %s from %s", tmp[i].mnt,
					group->grp_name);
			continue;
		}

		(*active)++;
	}

	ret = crt_req_decref(query_rpc);
	if (ret)
		IOF_TRACE_ERROR(iof_state, "Could not decrement ref count on "
				"query rpc");
	return 0;
}


static int iof_post_start(void *arg)
{
	struct iof_state *iof_state = arg;
	int ret;
	int grp_num;
	int total_projections = 0;
	int active_projections = 0;
	struct cnss_plugin_cb *cb;

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

		if (!group->attached)
			continue;

		ret = query_projections(iof_state, group, &total_projections,
					&active);

		if (ret) {
			IOF_TRACE_ERROR(iof_state, "Couldn't mount projections "
					"from %s", group->grp_name);
			continue;
		}
		active_projections += active;
	}

	cb->register_ctrl_constant_uint64(cb->plugin_dir,
					  "projection_count",
					  total_projections);

	if (total_projections == 0) {
		IOF_TRACE_ERROR(iof_state, "No projections found");
		return 1;
	}

	return 0;
}

/* Called once per projection, after the FUSE filesystem has been torn down */
static void iof_deregister_fuse(void *arg)
{
	struct iof_projection_info *fs_handle = arg;
	d_list_t *rlink = NULL;
	uint64_t refs = 0;
	int links = 0;
	int rc;

	IOF_TRACE_INFO(fs_handle, "Draining inode table");
	do {
		struct ioc_inode_entry *ie;

		rlink = d_chash_rec_first(&fs_handle->inode_ht);
		IOF_TRACE_DEBUG(fs_handle, "rlink is %p", rlink);
		if (!rlink)
			break;

		ie = container_of(rlink, struct ioc_inode_entry, list);

		refs += ie->ref;
		d_chash_rec_ndecref(&fs_handle->inode_ht, ie->ref, rlink);
		links++;
	} while (rlink);

	IOF_TRACE_INFO(fs_handle, "dropped %lu refs on %u handles",
		       refs, links);

	rc = d_chash_table_destroy_inplace(&fs_handle->inode_ht, false);
	if (rc)
		IOF_TRACE_WARNING(fs_handle, "Failed to close inode handles");

	iof_pool_destroy(&fs_handle->pool);

	IOF_TRACE_DOWN(fs_handle);
	d_list_del(&fs_handle->link);

	if (fs_handle->fuse_ops)
		D_FREE(fs_handle->fuse_ops);
	else
		D_FREE(fs_handle->fuse_ll_ops);

	D_FREE(fs_handle->base_dir);
	D_FREE(fs_handle->mount_point);
	D_FREE(fs_handle->stats);
	D_FREE(fs_handle);
}

static void iof_stop(void *arg)
{
	struct iof_state *iof_state = arg;
	struct iof_projection_info *fs_handle;

	IOF_TRACE_INFO(iof_state, "Called iof_stop");

	d_list_for_each_entry(fs_handle, &iof_state->fs_list, link) {
		IOF_TRACE_INFO(fs_handle, "Setting projection %d offline %s",
			       fs_handle->fs_id, fs_handle->mount_point);
		fs_handle->offline_reason = EACCES;
	}
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

		if (!group->attached) {
			iof_tracker_signal(&tracker);
			continue;
		}

		/*send a detach RPC to IONSS*/
		ret = crt_req_create(iof_state->crt_ctx, &group->grp.psr_ep,
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
		iof_wait(iof_state->crt_ctx, &tracker);

	for (i = 0; i < iof_state->num_groups; i++) {
		group = &iof_state->groups[i];

		D_FREE(group->grp_name);

		if (!group->attached)
			continue;

		ret = crt_group_detach(group->grp.dest_grp);
		if (ret)
			IOF_TRACE_ERROR(iof_state, "crt_group_detach failed "
					"with ret = %d", ret);
	}

	/* Stop progress thread */
	iof_thread_stop(iof_state);

	ret = crt_context_destroy(iof_state->crt_ctx, 0);
	if (ret)
		IOF_TRACE_ERROR(iof_state, "Could not destroy context");
	IOF_TRACE_INFO(iof_state, "Called iof_finish");

	IOF_TRACE_DOWN(iof_state);
	D_FREE(iof_state->groups);
	D_FREE(iof_state->cnss_prefix);
	D_FREE(iof_state);
}

struct cnss_plugin self = {.name                  = "iof",
			   .version               = CNSS_PLUGIN_VERSION,
			   .require_service       = 0,
			   .start                 = iof_reg,
			   .post_start            = iof_post_start,
			   .deregister_fuse       = iof_deregister_fuse,
			   .stop_plugin_services  = iof_stop,
			   .destroy_plugin_data   = iof_finish};

int iof_plugin_init(struct cnss_plugin **fns, size_t *size)
{
	struct iof_state *state;

	D_ALLOC_PTR(state);
	if (!state)
		return IOF_ERR_NOMEM;

	*size = sizeof(struct cnss_plugin);

	self.handle = state;
	*fns = &self;
	IOF_TRACE_UP(self.handle, *fns, "iof_state");
	return IOF_SUCCESS;
}
