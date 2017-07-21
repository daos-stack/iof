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
#ifdef IOF_USE_FUSE3
#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>
#else
#include <fuse/fuse.h>
#include <fuse/fuse_lowlevel.h>
#endif

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

	return (struct iof_projection_info *)context->private_data;
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
		IOF_LOG_INFO("Bad RPC reply %d", cb_info->cci_rc);
		reply->err = EIO;
		iof_tracker_signal(&reply->tracker);
		return;
	}

	if (out->err) {
		IOF_LOG_DEBUG("reply indicates error %d", out->err);
		reply->err = EIO;
	}
	reply->rc = out->rc;
	iof_tracker_signal(&reply->tracker);
}

/* Mark an endpoint as off-line, most likely because a process has been
 * evicted from the process set.
 *
 * Although this takes a endpoint_t option it's effectively only checking
 * the rank as each projection is only served from a single group and all
 * tags in a rank will be evicted simultaneously.
 *
 * This is called after a RPC reply is received so it might be invoked
 * multiple times for the same failure, and the dest_ep might have been
 * updated after the RPC was sent but before the RPC was rejected.
 */
void ioc_mark_ep_offline(struct iof_projection_info *fs_handle,
			 crt_endpoint_t *ep)
{
	/* If the projection is off-line then there is nothing to do */
	if (FS_IS_OFFLINE(fs_handle)) {
		IOF_LOG_INFO("FS %d already offline", fs_handle->fs_id);
		return;
	}

	/* If the projection has already migrated to another rank then
	 * there is nothing to do
	 */
	if (ep->ep_rank != fs_handle->dest_ep.ep_rank) {
		IOF_LOG_INFO("EP %d already offline for %d",
			     ep->ep_rank, fs_handle->fs_id);
		return;
	}

	/* Insert code to fail over to secondary EP here. */

	IOF_LOG_WARNING("Marking %d (%s) OFFLINE", fs_handle->fs_id,
			fs_handle->mount_point);

	fs_handle->offline_reason = EHOSTDOWN;
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
		IOF_LOG_INFO("Bad RPC reply %d", cb_info->cci_rc);
		reply->err = cb_info->cci_rc;
		iof_tracker_signal(&reply->tracker);
		return;
	}

	ret = crt_req_addref(cb_info->cci_rpc);
	if (ret) {
		IOF_LOG_ERROR("could not take reference on query RPC, ret = %d",
			      ret);
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
		IOF_LOG_ERROR("failed to create query rpc request, ret = %d",
				ret);
		return ret;
	}

	ret = crt_req_send(*query_rpc, query_cb, &reply);
	if (ret) {
		IOF_LOG_ERROR("Could not send query RPC, ret = %d", ret);
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

	cb = iof_state->cb;

	/* First check for the IONSS process set, and if it does not
	 * exist then * return cleanly to allow the rest of the CNSS
	 * code to run
	 */
	ret = crt_group_attach(group->grp_name, &group->grp.dest_grp);
	if (ret) {
		IOF_LOG_INFO("crt_group_attach failed with ret = %d",
			     ret);
		return ret;
	}

	ret = crt_group_config_save(group->grp.dest_grp);
	if (ret) {
		IOF_LOG_ERROR("crt_group_config_save failed for ionss "
			      "with ret = %d", ret);
		return ret;
	}

	/*initialize destination endpoint*/
	group->grp.psr_ep.ep_grp = 0; /*primary group*/
	/*TODO: Use exported PSR from cart*/
	group->grp.psr_ep.ep_rank = 0;
	group->grp.psr_ep.ep_tag = 0;
	group->grp.grp_id = id;

	sprintf(buf, "%d", id);
	ret = cb->create_ctrl_subdir(iof_state->ionss_dir, buf,
				     &ionss_dir);
	if (ret != 0) {
		IOF_LOG_ERROR("Failed to create control dir for ionss info "
			      "(rc = %d)\n", ret);
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

int dh_init(void *arg, void *handle)
{
	struct iof_dir_handle *dh = arg;

	dh->fs_handle = handle;
	dh->ep = dh->fs_handle->dest_ep;
	return 0;
}

int dh_clean(void *arg)
{
	struct iof_dir_handle *dh = arg;
	int rc;

	/* If there has been an error on the local handle, or readdir() is not
	 * exhausted then ensure that all resources are freed correctly
	 */
	if (dh->rpc)
		crt_req_decref(dh->rpc);
	dh->rpc = NULL;

	if (dh->name)
		free(dh->name);
	dh->name = NULL;

	if (dh->open_rpc)
		crt_req_decref(dh->open_rpc);

	if (dh->close_rpc)
		crt_req_decref(dh->close_rpc);

	rc = crt_req_create(dh->fs_handle->proj.crt_ctx, &dh->ep,
			    FS_TO_OP(dh->fs_handle, opendir), &dh->open_rpc);
	if (rc || !dh->open_rpc)
		return -1;

	rc = crt_req_create(dh->fs_handle->proj.crt_ctx, &dh->ep,
			    FS_TO_OP(dh->fs_handle, closedir), &dh->close_rpc);
	if (rc || !dh->close_rpc) {
		crt_req_decref(dh->open_rpc);
		return -1;
	}
	return 0;
}

void dh_release(void *arg)
{
	struct iof_dir_handle *dh = arg;

	crt_req_decref(dh->open_rpc);
	crt_req_decref(dh->close_rpc);
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
fh_clean(void *arg)
{
	struct iof_file_handle *fh = arg;
	int rc;

	if (fh->name)
		free(fh->name);
	fh->name = NULL;

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

	fh->common.ep = fh->fs_handle->dest_ep;

	rc = crt_req_create(fh->fs_handle->proj.crt_ctx, &fh->common.ep,
			    FS_TO_OP(fh->fs_handle, open), &fh->open_rpc);
	if (rc || !fh->open_rpc)
		return -1;

	rc = crt_req_create(fh->fs_handle->proj.crt_ctx, &fh->common.ep,
			    FS_TO_OP(fh->fs_handle, create), &fh->creat_rpc);
	if (rc || !fh->creat_rpc) {
		crt_req_decref(fh->open_rpc);
		return -1;
	}

	rc = crt_req_create(fh->fs_handle->proj.crt_ctx, &fh->common.ep,
			    FS_TO_OP(fh->fs_handle, close), &fh->release_rpc);
	if (rc || !fh->release_rpc) {
		crt_req_decref(fh->open_rpc);
		crt_req_decref(fh->creat_rpc);
		return -1;
	}

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
}

static int
gh_init(void *arg, void *handle)
{
	struct getattr_req *req = arg;

	req->fs_handle = handle;
	req->ep = req->fs_handle->dest_ep;
	return 0;
}

/* Clean, and prepare for use a getattr descriptor */
static int
gh_clean(void *arg)
{
	struct getattr_req *req = arg;
	struct iof_string_in *in;
	int rc;

	iof_tracker_init(&req->reply.tracker, 1);

	/* If this descriptor has previously been used the destroy the
	 * existing RPC
	 */
	if (req->rpc) {
		crt_req_decref(req->rpc);
		crt_req_decref(req->rpc);
		req->rpc = NULL;
	}

	/* Create a new RPC ready for later use.  Take an inital reference
	 * to the RPC so that it is not cleaned up after a successful send.
	 *
	 * After calling send the getattr code will re-take the dropped
	 * reference which means that on all subsequent calls to clean()
	 * or release() the ref count will be two.
	 *
	 * This means that both descriptor creation and destruction are
	 * done off the critical path.
	 */
	rc = crt_req_create(req->fs_handle->proj.crt_ctx, &req->ep,
			    FS_TO_OP(req->fs_handle, getattr), &req->rpc);
	if (rc || !req->rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u", rc);
		return -1;
	}
	crt_req_addref(req->rpc);
	in = crt_req_get(req->rpc);
	in->fs_id = req->fs_handle->fs_id;

	return 0;
}

/* Clean and prepare for use a getfattr descriptor */
static int
fgh_clean(void *arg)
{
	struct getattr_req *req = arg;
	int rc;

	iof_tracker_init(&req->reply.tracker, 1);

	if (req->rpc) {
		crt_req_decref(req->rpc);
		crt_req_decref(req->rpc);
		req->rpc = NULL;
	}

	rc = crt_req_create(req->fs_handle->proj.crt_ctx, &req->ep,
			    FS_TO_OP(req->fs_handle, getattr_gah), &req->rpc);
	if (rc || !req->rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u", rc);
		return -1;
	}
	crt_req_addref(req->rpc);

	return 0;
}

/* Destroy a descriptor which could be either getattr or getfattr */
static void
gh_release(void *arg)
{
	struct getattr_req *req = arg;

	crt_req_decref(req->rpc);
	crt_req_decref(req->rpc);
}

static int
rb_small_init(void *arg, void *handle)
{
	struct iof_rb *rb = arg;
	struct iof_projection_info *fs_handle = handle;

	rb->buf.buf[0].mem = calloc(1, fs_handle->max_iov_read);
	if (!rb->buf.buf[0].mem)
		return -1;

	rb->buf.count = 1;
	rb->buf.buf[0].fd = -1;

	return 0;
}

static int
rb_page_init(void *arg, void *handle)
{
	struct iof_rb *rb = arg;

	rb->buf.buf[0].mem = calloc(1, 4096);
	if (!rb->buf.buf[0].mem)
		return -1;

	rb->buf.count = 1;
	rb->buf.buf[0].fd = -1;

	return 0;
}

static int
rb_large_init(void *arg, void *handle)
{
	struct iof_rb *rb = arg;
	struct iof_projection_info *fs_handle = handle;

	rb->buf.buf[0].mem = calloc(1, fs_handle->max_read);
	if (!rb->buf.buf[0].mem)
		return -1;

	rb->buf.count = 1;
	rb->buf.buf[0].fd = -1;

	return 0;
}

static void
rb_release(void *arg)
{
	struct iof_rb *rb = arg;

	free(rb->buf.buf[0].mem);
}

static int iof_reg(void *arg, struct cnss_plugin_cb *cb, size_t cb_size)
{
	struct iof_state *iof_state = arg;
	struct iof_group_info *group;
	char *prefix;
	int ret;
	DIR *prefix_dir;
	int num_attached = 0;
	int i;

	iof_state->cb = cb;

	/* Hard code only the default group now */
	iof_state->num_groups = 1;
	iof_state->groups = calloc(1, sizeof(struct iof_group_info));
	if (iof_state->groups == NULL) {
		IOF_LOG_ERROR("No memory available to configure IONSS");
		return IOF_ERR_NOMEM;
	}

	group = &iof_state->groups[0];
	group->grp_name = strdup(IOF_DEFAULT_SET);
	if (group->grp_name == NULL) {
		IOF_LOG_ERROR("No memory available to configure IONSS");
		free(iof_state->groups);
		return IOF_ERR_NOMEM;
	}

	CRT_INIT_LIST_HEAD(&iof_state->fs_list);

	cb->register_ctrl_constant_uint64(cb->plugin_dir, "ionss_count", 1);
	ret = cb->create_ctrl_subdir(cb->plugin_dir, "ionss",
				     &iof_state->ionss_dir);
	if (ret != 0) {
		IOF_LOG_ERROR("Failed to create control dir for ionss info"
			      "(rc = %d)\n", ret);
		return IOF_ERR_CTRL_FS;
	}

	/* Despite the hard coding above, now we can do attaches in a loop */
	for (i = 0; i < iof_state->num_groups; i++) {
		group = &iof_state->groups[i];

		ret = attach_group(iof_state, group, i);
		if (ret != 0) {
			IOF_LOG_ERROR("Failed to attach to service group"
				      " %s (ret = %d)", group->grp_name, ret);
			free(group->grp_name);
			group->grp_name = NULL;
			continue;
		}

		num_attached++;
	}

	if (num_attached == 0) {
		IOF_LOG_ERROR("No IONSS found");
		free(iof_state->groups);
		return 1;
	}

	cb->register_ctrl_constant_uint64(cb->plugin_dir, "ioctl_version",
					  IOF_IOCTL_VERSION);

	ret = crt_context_create(NULL, &iof_state->crt_ctx);
	if (ret)
		IOF_LOG_ERROR("Context not created");

	prefix = getenv("CNSS_PREFIX");
	iof_state->cnss_prefix = realpath(prefix, NULL);
	prefix_dir = opendir(iof_state->cnss_prefix);
	if (prefix_dir)
		closedir(prefix_dir);
	else {
		if (mkdir(iof_state->cnss_prefix, 0755)) {
			IOF_LOG_ERROR("Could not create cnss_prefix");
			return CNSS_ERR_PREFIX;
		}
	}

	/*registrations*/
	ret = crt_rpc_register(QUERY_PSR_OP, &QUERY_RPC_FMT);
	if (ret) {
		IOF_LOG_ERROR("Query rpc registration failed with ret: %d",
				ret);
		return ret;
	}

	ret = crt_rpc_register(DETACH_OP, NULL);
	if (ret) {
		IOF_LOG_ERROR("Detach registration failed with ret: %d", ret);
		return ret;
	}

	iof_register(DEF_PROTO_CLASS(DEFAULT), NULL);
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

#if IOF_USE_FUSE3
#define REGISTER_STAT3(_STAT) REGISTER_STAT(_STAT)
#else
#define REGISTER_STAT3(_STAT) (void)0
#endif

static int initialize_projection(struct iof_state *iof_state,
				 struct iof_group_info *group,
				 struct iof_fs_info *fs_info,
				 struct iof_psr_query *query,
				 int id)
{
	struct iof_projection_info *fs_handle;
	struct cnss_plugin_cb *cb;
	struct fuse_args args = {0};
	void *argv;
	char *base_name;
	char *read_option = NULL;
	char const *opts[] = {"-o", "fsname=IOF",
			      "-o", "subtype=pam",
#if !IOF_USE_FUSE3
			      "-o", "use_ino",
			      "-o", "entry_timeout=0",
			      "-o", "negative_timeout=0",
			      "-o", "attr_timeout=0",
#endif
	};
	int ret;

	cb = iof_state->cb;

	/* TODO: This is presumably wrong although it's not
	 * clear how best to handle it
	 */
	if (!iof_is_mode_supported(fs_info->flags))
		return IOF_NOT_SUPP;

	fs_handle = calloc(1,
			   sizeof(struct iof_projection_info));
	if (!fs_handle)
		return IOF_ERR_NOMEM;

	ret = iof_pool_init(&fs_handle->pool);
	if (ret != 0) {
		free(fs_handle);
		return IOF_ERR_NOMEM;
	}

	fs_handle->iof_state = iof_state;
	fs_handle->flags = fs_info->flags;
	IOF_LOG_INFO("Filesystem mode: Private");

	fs_handle->max_read = query->max_read;
	fs_handle->max_iov_read = query->max_iov_read;
	fs_handle->max_write = query->max_write;
	fs_handle->readdir_size = query->readdir_size;

	base_name = basename(fs_info->mnt);

	ret = asprintf(&fs_handle->mount_point, "%s/%s",
		       iof_state->cnss_prefix, base_name);
	if (ret == -1)
		return IOF_ERR_NOMEM;

	IOF_LOG_DEBUG("Projected Mount %s", base_name);

	IOF_LOG_INFO("Mountpoint for this projection: %s",
		     fs_handle->mount_point);

	fs_handle->fs_id = fs_info->id;
	fs_handle->proj.cli_fs_id = id;

	fs_handle->stats = calloc(1, sizeof(*fs_handle->stats));
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
					  "max_write",
					  fs_handle->max_write);

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
	REGISTER_STAT(chmod);
	REGISTER_STAT(create);
	REGISTER_STAT(readlink);
	REGISTER_STAT(rmdir);
	REGISTER_STAT(mkdir);
	REGISTER_STAT(statfs);
	REGISTER_STAT(unlink);
	REGISTER_STAT(ioctl);
	REGISTER_STAT(open);
	REGISTER_STAT(release);
	REGISTER_STAT(symlink);
	REGISTER_STAT(rename);
	REGISTER_STAT(truncate);
	REGISTER_STAT(utimens);
	REGISTER_STAT(read);
	REGISTER_STAT(write);

	REGISTER_STAT64(read_bytes);
	REGISTER_STAT64(write_bytes);

	REGISTER_STAT3(getfattr);
	REGISTER_STAT3(ftruncate);
	REGISTER_STAT3(fchmod);
	REGISTER_STAT3(futimens);

	REGISTER_STAT(il_ioctl);

	IOF_LOG_INFO("Filesystem ID srv:%d cli:%d",
		     fs_handle->fs_id,
		     fs_handle->proj.cli_fs_id);

	fs_handle->proj.grp = &group->grp;
	fs_handle->dest_ep = group->grp.psr_ep;
	fs_handle->proj.grp_id = group->grp.grp_id;
	fs_handle->proj.crt_ctx = iof_state->crt_ctx;
	fs_handle->fuse_ops = iof_get_fuse_ops(fs_handle->flags);

	args.argc = (sizeof(opts) / sizeof(*opts)) + 2;
	args.argv = calloc(args.argc, sizeof(char *));
	argv = args.argv;
	args.argv[0] = (char *)&"";
	memcpy(&args.argv[2], opts, sizeof(opts));

	ret = asprintf(&read_option, "-omax_read=%u",
		       fs_handle->max_read);
	if (ret == -1)
		return IOF_ERR_NOMEM;

	args.argv[1] = read_option;

	ret = cb->register_fuse_fs(cb->handle,
				   fs_handle->fuse_ops, &args,
				   fs_handle->mount_point,
				   fs_handle->flags & IOF_CNSS_MT,
				   fs_handle);
	if (ret) {
		IOF_LOG_ERROR("Unable to register FUSE fs");
		free(fs_handle);
		return 1;
	}

	free(read_option);
	free(argv);

	{
		/* Register the directory handle type
		 *
		 * This is done late on in the registraction as the dh_int()
		 * and dh_clean() functions require access to fs_handle.
		 */
		struct iof_pool_reg pt = { .handle = fs_handle,
					   .init = dh_init,
					   .clean = dh_clean,
					   .release = dh_release,
					   POOL_TYPE_INIT(iof_dir_handle, list)};

		struct iof_pool_reg fh = { .handle = fs_handle,
					   .init = fh_init,
					   .clean = fh_clean,
					   .release = fh_release,
					   POOL_TYPE_INIT(iof_file_handle, list)};

		struct iof_pool_reg gt = { .handle = fs_handle,
					   .init = gh_init,
					   .clean = gh_clean,
					   .release = gh_release,
					   POOL_TYPE_INIT(getattr_req, list)};

		struct iof_pool_reg fgt = { .handle = fs_handle,
					    .init = gh_init,
					    .clean = fgh_clean,
					    .release = gh_release,
					    POOL_TYPE_INIT(getattr_req, list)};

		struct iof_pool_reg rb_small = { .handle = fs_handle,
						 .init = rb_small_init,
						 .release = rb_release,
						 POOL_TYPE_INIT(iof_rb, list)};

		struct iof_pool_reg rb_page = { .handle = fs_handle,
						 .init = rb_page_init,
						 .release = rb_release,
						 POOL_TYPE_INIT(iof_rb, list)};

		struct iof_pool_reg rb_large = { .handle = fs_handle,
						 .init = rb_large_init,
						 .release = rb_release,
						 POOL_TYPE_INIT(iof_rb, list)};

		fs_handle->dh = iof_pool_register(&fs_handle->pool, &pt);
		if (!fs_handle->dh)
			return IOF_ERR_NOMEM;

		fs_handle->gh_pool = iof_pool_register(&fs_handle->pool, &gt);
		if (!fs_handle->gh_pool)
			return IOF_ERR_NOMEM;

		fs_handle->fgh_pool = iof_pool_register(&fs_handle->pool, &fgt);
		if (!fs_handle->fgh_pool)
			return IOF_ERR_NOMEM;

		fs_handle->fh_pool = iof_pool_register(&fs_handle->pool, &fh);
		if (!fs_handle->fh_pool)
			return IOF_ERR_NOMEM;

		fs_handle->rb_pool_small = iof_pool_register(&fs_handle->pool,
							     &rb_small);
		if (!fs_handle->rb_pool_small)
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

	IOF_LOG_DEBUG("Fuse mount installed at: %s",
		      fs_handle->mount_point);

	crt_list_add_tail(&fs_handle->link, &iof_state->fs_list);

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
		IOF_LOG_ERROR("Query operation failed");
		return IOF_ERR_PROJECTION;
	}

	/*calculate number of projections*/
	fs_num = (query->query_list.iov_len)/sizeof(struct iof_fs_info);
	IOF_LOG_DEBUG("Number of filesystems projected by %s: %d",
		      group->grp_name, fs_num);
	tmp = (struct iof_fs_info *) query->query_list.iov_buf;

	for (i = 0; i < fs_num; i++) {
		ret = initialize_projection(iof_state, group, &tmp[i], query,
					    (*total)++);

		if (ret != 0) {
			IOF_LOG_ERROR("Could not initialize projection %s from"
				      " %s", tmp[i].mnt, group->grp_name);
			continue;
		}

		(*active)++;
	}

	ret = crt_req_decref(query_rpc);
	if (ret)
		IOF_LOG_ERROR("Could not decrement ref count on query rpc");

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
		IOF_LOG_ERROR("Failed to create control dir for PA mode "
			      "(rc = %d)\n", ret);
		return IOF_ERR_CTRL_FS;
	}


	for (grp_num = 0; grp_num < iof_state->num_groups; grp_num++) {
		struct iof_group_info *group = &iof_state->groups[grp_num];
		int active;

		ret = query_projections(iof_state, group, &total_projections,
					&active);

		if (ret) {
			IOF_LOG_ERROR("Couldn't mount projections from %s",
				      group->grp_name);
			continue;
		}

		active_projections += active;
	}

	cb->register_ctrl_constant_uint64(cb->plugin_dir,
					  "projection_count",
					  total_projections);

	if (total_projections == 0) {
		IOF_LOG_ERROR("No projections found");
		return 1;
	}

	return 0;
}

/* Called once per projection, after the FUSE filesystem has been torn down */
static void iof_deregister_fuse(void *arg)
{
	struct iof_projection_info *fs_handle = arg;

	iof_pool_destroy(&fs_handle->pool);

	crt_list_del(&fs_handle->link);
	free(fs_handle->fuse_ops);
	free(fs_handle->base_dir);
	free(fs_handle->mount_point);
	free(fs_handle->stats);
	free(fs_handle);
}

static void iof_stop(void *arg)
{
	struct iof_state *iof_state = arg;
	struct iof_projection_info *fs_handle;

	IOF_LOG_INFO("Called iof_stop");

	crt_list_for_each_entry(fs_handle, &iof_state->fs_list, link) {
		IOF_LOG_INFO("Setting projection %d offline %s",
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
	crt_rpc_t *rpc;
	int ret;
	int i;
	struct iof_tracker tracker;

	iof_tracker_init(&tracker, iof_state->num_groups);

	for (i = 0; i < iof_state->num_groups; i++) {
		rpc = NULL;
		group = &iof_state->groups[i];
		/*send a detach RPC to IONSS*/
		ret = crt_req_create(iof_state->crt_ctx, &group->grp.psr_ep,
				     DETACH_OP, &rpc);
		if (ret || !rpc) {
			IOF_LOG_ERROR("Could not create detach req ret = %d",
				      ret);
			iof_tracker_signal(&tracker);
			continue;
		}

		ret = crt_req_send(rpc, detach_cb, &tracker);
		if (ret) {
			IOF_LOG_ERROR("Detach RPC not sent");
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

		ret = crt_group_detach(group->grp.dest_grp);
		if (ret)
			IOF_LOG_ERROR("crt_group_detach failed with ret = %d",
				      ret);
		free(group->grp_name);
	}

	ret = crt_context_destroy(iof_state->crt_ctx, 0);
	if (ret)
		IOF_LOG_ERROR("Could not destroy context");
	IOF_LOG_INFO("Called iof_finish with %p", iof_state);

	free(iof_state->groups);
	free(iof_state->cnss_prefix);
	free(iof_state);
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
	*size = sizeof(struct cnss_plugin);

	self.handle = calloc(1, sizeof(struct iof_state));
	if (!self.handle)
		return IOF_ERR_NOMEM;
	*fns = &self;
	return IOF_SUCCESS;
}
