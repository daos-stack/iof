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
#include "iof.h"
#include "log.h"
#include "ios_gah.h"
#include "iof_ioctl.h"

struct query_cb_r {
	int complete;
	int err;
	struct iof_psr_query **query;
};

struct fs_handle *ioc_get_handle(void)
{
	struct fuse_context *context = fuse_get_context();

	return (struct fs_handle *)context->private_data;
}

static int iof_check_complete(void *arg)
{
	int *complete = (int *)arg;
	return *complete;
}

/* on-demand progress */
static int iof_progress(crt_context_t crt_ctx, int *complete_flag)
{
	int		rc;

	do {
		rc = crt_progress(crt_ctx, 1000 * 1000, iof_check_complete,
				  complete_flag);

		if (*complete_flag)
			return 0;

	} while (rc == 0 || rc == -CER_TIMEDOUT);

	IOF_LOG_ERROR("crt_progress failed rc: %d", rc);
	return -1;
}

/* Progress, from within FUSE callbacks during normal I/O
 *
 * This will use a default set of values to iof_progress, and do the correct
 * thing on timeout, potentially shutting dowth the filesystem if there is
 * a problem.
 */
int ioc_cb_progress(struct fs_handle *fs_handle, int *complete_flag)
{
	int rc;

	rc = iof_progress(fs_handle->crt_ctx, complete_flag);
	if (rc) {
		/* TODO: check is PSR is alive before exiting fuse */
		IOF_LOG_ERROR("exiting fuse loop rc %d", rc);
#if 0
		fuse_session_exit(fuse_get_session(fs_handle->fuse));
#endif
		return EINTR;
	}
	return 0;
}

/*
 * A common callback that is used by several of the I/O RPCs that only return
 * status, with no data or metadata, for example rmdir and truncate.
 *
 * out->err will always be a errno that can be passed back to FUSE.
 */
int ioc_status_cb(const struct crt_cb_info *cb_info)
{
	struct status_cb_r *reply = NULL;
	struct iof_status_out *out = NULL;
	crt_rpc_t *rpc = cb_info->cci_rpc;

	reply = (struct status_cb_r *)cb_info->cci_arg;

	if (cb_info->cci_rc != 0) {
		/*
		 * Error handling.  Return EIO on any error
		 */
		IOF_LOG_INFO("Bad RPC reply %d", cb_info->cci_rc);
		reply->err = EIO;
		reply->complete = 1;
		return 0;
	}

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not get output");
		reply->err = EIO;
		reply->complete = 1;
		return 0;
	}

	if (out->err) {
		IOF_LOG_DEBUG("reply indicates error %d", out->err);
		reply->err = EIO;
	}
	reply->rc = out->rc;
	reply->complete = 1;
	return 0;
}

static int query_callback(const struct crt_cb_info *cb_info)
{
	struct query_cb_r *reply;
	int ret;
	struct iof_psr_query *query;
	crt_rpc_t *query_rpc;

	query_rpc = cb_info->cci_rpc;
	reply = (struct query_cb_r *) cb_info->cci_arg;

	if (cb_info->cci_rc != 0) {
		/*
		 * Error handling.  On timeout return EAGAIN, all other errors
		 * return EIO.
		 *
		 * TODO: Handle target eviction here
		 */
		IOF_LOG_INFO("Bad RPC reply %d", cb_info->cci_rc);
		reply->err = cb_info->cci_rc;
		reply->complete = 1;
		return 0;
	}

	query = crt_reply_get(query_rpc);
	if (!query) {
		IOF_LOG_ERROR("Could not get query reply");
		reply->complete = 1;
		return 0;
	}

	ret = crt_req_addref(query_rpc);
	if (ret) {
		IOF_LOG_ERROR("could not take reference on query RPC, ret = %d",
				ret);
		reply->complete = 1;
		return 0;
	}

	*reply->query = query;

	reply->complete = 1;
	return 0;
}

/*Send RPC to PSR to get information about projected filesystems*/
static int ioc_get_projection_info(struct iof_state *iof_state,
				   struct iof_psr_query **query,
				   crt_rpc_t **query_rpc)
{
	int ret;
	struct query_cb_r reply = {0};

	reply.complete = 0;
	reply.query = query;

	ret = crt_req_create(iof_state->crt_ctx, iof_state->psr_ep,
			     QUERY_PSR_OP, query_rpc);
	if (ret || (*query_rpc == NULL)) {
		IOF_LOG_ERROR("failed to create query rpc request, ret = %d",
				ret);
		return ret;
	}

	ret = crt_req_send(*query_rpc, query_callback, &reply);
	if (ret) {
		IOF_LOG_ERROR("Could not send query RPC, ret = %d", ret);
		return ret;
	}

	/*make on-demand progress*/
	ret = iof_progress(iof_state->crt_ctx, &reply.complete);
	if (ret)
		IOF_LOG_ERROR("Could not complete PSR query");

	if (reply.err)
		return reply.err;

	return ret;
}

static int iof_uint_read(char *buf, size_t buflen, void *arg)
{
	uint *value = (uint *)arg;

	snprintf(buf, buflen, "%u", *value);
	return 0;
}

static int iof_uint64_read(char *buf, size_t buflen, void *arg)
{
	uint64_t *value = (uint64_t *)arg;

	snprintf(buf, buflen, "%lu", *value);
	return 0;
}

int iof_reg(void *arg, struct cnss_plugin_cb *cb, size_t cb_size)
{
	struct iof_state *iof_state = (struct iof_state *)arg;
	crt_group_t *ionss_group;
	char *prefix;
	int ret;
	DIR *prefix_dir;
	struct ctrl_dir *ionss_0_dir = NULL;


	/* First check for the IONSS process set, and if it does not exist then
	 * return cleanly to allow the rest of the CNSS code to run
	 */
	ret = crt_group_attach(IOF_DEFAULT_SET, &ionss_group);
	if (ret) {
		IOF_LOG_INFO("crt_group_attach failed with ret = %d", ret);
		return ret;
	}

	/*do a group lookup*/
	iof_state->dest_group = ionss_group;

	/*initialize destination endpoint*/
	iof_state->psr_ep.ep_grp = 0; /*primary group*/
	/*TODO: Use exported PSR from cart*/
	iof_state->psr_ep.ep_rank = 0;
	iof_state->psr_ep.ep_tag = 0;

	cb->register_ctrl_constant_uint64(cb->plugin_dir, "ionss_count", 1);
	ret = cb->create_ctrl_subdir(cb->plugin_dir, "ionss",
				     &iof_state->ionss_dir);
	if (ret != 0) {
		IOF_LOG_ERROR("Failed to create control dir for ionss info"
			      "(rc = %d)\n", ret);
		return IOF_ERR_CTRL_FS;
	}

	ret = cb->create_ctrl_subdir(iof_state->ionss_dir, "0",
				     &ionss_0_dir);
	if (ret != 0) {
		IOF_LOG_ERROR("Failed to create control dir for ionss info "
			      "(rc = %d)\n", ret);
		return IOF_ERR_CTRL_FS;
	}
	cb->register_ctrl_constant_uint64(ionss_0_dir, "psr_rank",
					  iof_state->psr_ep.ep_rank);
	cb->register_ctrl_constant_uint64(ionss_0_dir, "psr_tag",
					  iof_state->psr_ep.ep_tag);
	cb->register_ctrl_constant(ionss_0_dir, "name", IOF_DEFAULT_SET);
	cb->register_ctrl_constant_uint64(cb->plugin_dir, "ioctl_version",
					  IOF_IOCTL_VERSION);

	ret = crt_context_create(NULL, &iof_state->crt_ctx);
	if (ret)
		IOF_LOG_ERROR("Context not created");

	ret = crt_group_config_save(ionss_group);
	if (ret) {
		IOF_LOG_ERROR("crt_group_config_save failed for ionss "
			      "with ret = %d", ret);
		return ret;
	}

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
	iof_state->cb = cb;
	iof_state->cb_size = cb_size;

	return ret;
}

#define REGISTER_STAT(_STAT) cb->register_ctrl_variable( \
		fs_handle->stats_dir,					\
		#_STAT,							\
		iof_uint_read,						\
		NULL, NULL,						\
		&fs_handle->stats->_STAT)
#define REGISTER_STAT64(_STAT) cb->register_ctrl_variable( \
		fs_handle->stats_dir,					\
		#_STAT,							\
		iof_uint64_read,					\
		NULL, NULL,						\
		&fs_handle->stats->_STAT)

#if IOF_USE_FUSE3
#define REGISTER_STAT3(_STAT) REGISTER_STAT(_STAT)
#else
#define REGISTER_STAT3(_STAT) (void)0
#endif

int iof_post_start(void *arg)
{
	struct iof_state *iof_state = (struct iof_state *)arg;
	struct iof_psr_query *query = NULL;
	int ret;
	int i;
	int fs_num;
	struct cnss_plugin_cb *cb;

	struct iof_fs_info *tmp;
	crt_rpc_t *query_rpc = NULL;

	char const *opts[] = {"-ofsname=IOF",
			      "-osubtype=pam",
#if !IOF_USE_FUSE3
			      "-o", "use_ino",
			      "-o", "entry_timeout=0",
			      "-o", "negative_timeout=0",
			      "-o", "attr_timeout=0",
#endif
	};

	cb = iof_state->cb;

	/*Query PSR*/
	ret = ioc_get_projection_info(iof_state, &query, &query_rpc);
	if (ret || (query == NULL)) {
		IOF_LOG_ERROR("Query operation failed");
		return IOF_ERR_PROJECTION;
	}

	ret = cb->create_ctrl_subdir(cb->plugin_dir, "projections",
				     &iof_state->projections_dir);
	if (ret != 0) {
		IOF_LOG_ERROR("Failed to create control dir for PA mode "
			      "(rc = %d)\n", ret);
		return IOF_ERR_CTRL_FS;
	}

	/*calculate number of projections*/
	fs_num = (query->query_list.iov_len)/sizeof(struct iof_fs_info);
	IOF_LOG_DEBUG("Number of filesystems projected: %d", fs_num);

	tmp = (struct iof_fs_info *) query->query_list.iov_buf;

	for (i = 0; i < fs_num; i++) {
		struct fs_handle *fs_handle;
		struct fuse_args args = {0};
		void *argv;
		char *base_name;
		char *read_option = NULL;

		/* TODO: This is presumably wrong although it's not clear
		 * how best to handle it
		 */
		if (!iof_is_mode_supported(tmp[i].flags))
			return IOF_NOT_SUPP;

		fs_handle = calloc(1, sizeof(struct fs_handle));
		if (!fs_handle)
			return IOF_ERR_NOMEM;
		fs_handle->iof_state = iof_state;
		fs_handle->flags = tmp[i].flags;
		IOF_LOG_INFO("Filesystem mode: Private");

		fs_handle->max_read = query->max_read;
		fs_handle->max_write = query->max_write;

		base_name = basename(tmp[i].mnt);

		ret = asprintf(&fs_handle->mount_point, "%s/%s",
			       iof_state->cnss_prefix, base_name);
		if (ret == -1)
			return IOF_ERR_NOMEM;

		IOF_LOG_DEBUG("Projected Mount %s", base_name);

		IOF_LOG_INFO("Mountpoint for this projection: %s",
			     fs_handle->mount_point);

		fs_handle->fs_id = tmp[i].id;

		fs_handle->stats = calloc(1, sizeof(*fs_handle->stats));
		if (!fs_handle->stats)
			return 1;

		ret = asprintf(&fs_handle->base_dir, "%d", fs_handle->fs_id);
		if (ret == -1)
			return IOF_ERR_NOMEM;

		cb->create_ctrl_subdir(iof_state->projections_dir,
				       fs_handle->base_dir,
				       &fs_handle->fs_dir);

		/*Register the mount point with the control filesystem*/
		cb->register_ctrl_constant(fs_handle->fs_dir, "mount_point",
					   fs_handle->mount_point);

		cb->register_ctrl_constant(fs_handle->fs_dir, "mode",
					   "private");

		cb->register_ctrl_constant_uint64(fs_handle->fs_dir, "max_read",
						  fs_handle->max_read);

		cb->register_ctrl_constant_uint64(fs_handle->fs_dir,
						  "max_write",
						  fs_handle->max_read);

		cb->create_ctrl_subdir(fs_handle->fs_dir, "stats",
				       &fs_handle->stats_dir);

		REGISTER_STAT(opendir);
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

		IOF_LOG_INFO("Filesystem ID %d", fs_handle->fs_id);

		fs_handle->dest_ep = iof_state->psr_ep;
		fs_handle->crt_ctx = iof_state->crt_ctx;
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

		ret = cb->register_fuse_fs(cb->handle, fs_handle->fuse_ops,
					   &args, fs_handle->mount_point,
					   fs_handle);
		if (ret) {
			IOF_LOG_ERROR("Unable to register FUSE fs");
			free(fs_handle);
			return 1;
		}

		free(read_option);
		free(argv);

		IOF_LOG_DEBUG("Fuse mount installed at: %s",
			      fs_handle->mount_point);
	}

	ret = crt_req_decref(query_rpc);
	if (ret)
		IOF_LOG_ERROR("Could not decrement ref count on query rpc");

	return ret;
}

/* Called once per projection, after the FUSE filesystem has been torn down */
void iof_deregister_fuse(void *arg)
{
	struct fs_handle *fs_handle = (struct fs_handle *)arg;

	free(fs_handle->fuse_ops);
	free(fs_handle->base_dir);
	free(fs_handle->mount_point);
	free(fs_handle->stats);
	free(fs_handle);
}

void iof_flush(void *arg)
{

	IOF_LOG_INFO("Called iof_flush");

}

static int detach_cb(const struct crt_cb_info *cb_info)
{
	int *complete;

	complete = (int *)cb_info->cci_arg;

	*complete = 1;

	return IOF_SUCCESS;
}

void iof_finish(void *arg)
{
	struct iof_state *iof_state = (struct iof_state *)arg;
	int ret;
	crt_rpc_t *rpc = NULL;
	int complete;

	/*send a detach RPC to IONSS*/
	ret = crt_req_create(iof_state->crt_ctx, iof_state->psr_ep,
			     DETACH_OP, &rpc);
	if (ret || !rpc)
		IOF_LOG_ERROR("Could not create detach request ret = %d",
				ret);

	complete = 0;
	ret = crt_req_send(rpc, detach_cb, &complete);
	if (ret)
		IOF_LOG_ERROR("Detach RPC not sent");

	ret = iof_progress(iof_state->crt_ctx, &complete);
	if (ret)
		IOF_LOG_ERROR("Could not progress detach RPC");
	ret = crt_context_destroy(iof_state->crt_ctx, 0);
	if (ret)
		IOF_LOG_ERROR("Could not destroy context");
	IOF_LOG_INFO("Called iof_finish with %p", iof_state);

	ret = crt_group_detach(iof_state->dest_group);
	if (ret)
		IOF_LOG_ERROR("crt_group_detach failed with ret = %d", ret);

	free(iof_state->cnss_prefix);
	free(iof_state);
}

struct cnss_plugin self = {.name            = "iof",
			   .version         = CNSS_PLUGIN_VERSION,
			   .require_service = 0,
			   .start           = iof_reg,
			   .post_start      = iof_post_start,
			   .deregister_fuse = iof_deregister_fuse,
			   .flush           = iof_flush,
			   .finish          = iof_finish};

int iof_plugin_init(struct cnss_plugin **fns, size_t *size)
{
	*size = sizeof(struct cnss_plugin);

	self.handle = calloc(1, sizeof(struct iof_state));
	if (!self.handle)
		return IOF_ERR_NOMEM;
	*fns = &self;
	return IOF_SUCCESS;
}

