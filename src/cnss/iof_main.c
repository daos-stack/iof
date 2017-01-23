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

/* Handle passed to all CNSS callbacks */
struct iof_handle {
	struct iof_state *state;
	struct cnss_plugin_cb *cb;
};

struct query_cb_r {
	int complete;
	struct iof_psr_query **query;
};

static int string_to_bool(const char *str, int *value)
{
	if (strcmp(str, "1") == 0) {
		*value = 1;
		return 1;
	} else if (strcmp(str, "0") == 0) {
		*value = 0;
		return 1;
	}
	return 0;
}

/* on-demand progress */
static int iof_progress(crt_context_t crt_ctx, int num_retries,
			unsigned int wait_len_ms, int *complete_flag)
{
	int		retry;
	int		rc;

	for (retry = 0; retry < num_retries; retry++) {
		rc = crt_progress(crt_ctx, wait_len_ms * 1000, NULL, NULL);
		if (rc != 0 && rc != -CER_TIMEDOUT) {
			IOF_LOG_ERROR("crt_progress failed rc: %d", rc);
			break;
		}
		if (*complete_flag)
			return 0;
		sched_yield();
	}
	return -ETIMEDOUT;
}

/* Progress, from within FUSE callbacks during normal I/O
 *
 * This will use a default set of values to iof_progress, and do the correct
 * thing on timeout, potentially shutting dowth the filesystem if there is
 * a problem.
 */
int ioc_cb_progress(crt_context_t crt_ctx, struct fuse_context *context,
		    int *complete_flag)
{
	int rc;

	rc = iof_progress(crt_ctx, 50, 6000, complete_flag);
	if (rc) {
		/*TODO: check is PSR is alive before exiting fuse*/
		IOF_LOG_ERROR("exiting fuse loop");
		fuse_session_exit(fuse_get_session(context->fuse));
		return EINTR;
	}
	return 0;
}

#if IOF_USE_FUSE3
static int ioc_getattr3(const char *path, struct stat *stbuf,
			struct fuse_file_info *fi)
{
	if (fi)
		return -EIO;

	if (!path)
		return -EIO;
	return ioc_getattr(path, stbuf);
}
#endif

/*
 * Temporary way of shutting down. This fuse callback is currently not going to
 * IONSS
 */
#ifdef __APPLE__

static int ioc_setxattr(const char *path,  const char *name, const char *value,
			size_t size, int options, uint32_t position)
#else
static int ioc_setxattr(const char *path, const char *name, const char *value,
			size_t size, int flags)
#endif
{
	struct fuse_context *context;
	int ret;

	context = fuse_get_context();
	if (strcmp(name, "user.exit") == 0) {
		if (string_to_bool(value, &ret)) {
			if (ret) {
				IOF_LOG_INFO("Exiting fuse loop");
				fuse_session_exit(
					fuse_get_session(context->fuse));
				return -EINTR;
			} else
				return 0;
		}
		return -EINVAL;
	}
	return -ENOTSUP;
}





static struct fuse_operations ops = {
#if IOF_USE_FUSE3
	.getattr = ioc_getattr3,
#else
#ifndef __APPLE__
	.flag_nopath = 1,
#endif
	.getattr = ioc_getattr,
#endif
	.opendir = ioc_opendir,
	.readdir = ioc_readdir,
	.releasedir = ioc_closedir,
	.setxattr = ioc_setxattr,
	.open = ioc_open,
	.release = ioc_release,
	.create = ioc_create,
	.read = ioc_read,
};

static int query_callback(const struct crt_cb_info *cb_info)
{
	struct query_cb_r *reply;
	int ret;
	crt_rpc_t *query_rpc;

	query_rpc = cb_info->cci_rpc;
	reply = (struct query_cb_r *) cb_info->cci_arg;

	*reply->query = crt_reply_get(query_rpc);
	if (*reply->query == NULL) {
		IOF_LOG_ERROR("Could not get query reply");
		return IOF_ERR_CART;
	}

	ret = crt_req_addref(query_rpc);
	if (ret)
		IOF_LOG_ERROR("could not take reference on query RPC, ret = %d",
				ret);
	reply->complete = 1;
	return ret;
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

	ret = crt_req_create(iof_state->crt_ctx, iof_state->dest_ep,
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
	ret = iof_progress(iof_state->crt_ctx, 50, 6000, &reply.complete);
	if (ret)
		IOF_LOG_ERROR("Could not complete PSR query");

	return ret;
}


int iof_reg(void *foo, struct cnss_plugin_cb *cb,
	    size_t cb_size)
{
	struct iof_handle *handle = (struct iof_handle *)foo;
	struct iof_state *iof_state;
	crt_group_t *ionss_group;
	char *prefix;
	int ret;
	DIR *prefix_dir;

	/* First check for the IONSS process set, and if it does not exist then
	 * return cleanly to allow the rest of the CNSS code to run
	 */
	ret = crt_group_attach("IONSS", &ionss_group);
	if (ret) {
		IOF_LOG_INFO("crt_group_attach failed with ret = %d", ret);
		return ret;
	}

	if (!handle->state) {
		handle->state = calloc(1, sizeof(struct iof_state));
		if (!handle->state)
			return IOF_ERR_NOMEM;
	}

	/*initialize iof state*/
	iof_state = handle->state;
	/*do a group lookup*/
	iof_state->dest_group = ionss_group;

	/*initialize destination endpoint*/
	iof_state->dest_ep.ep_grp = 0; /*primary group*/
	/*TODO: Use exported PSR from cart*/
	iof_state->dest_ep.ep_rank = 0;
	iof_state->dest_ep.ep_tag = 0;

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

	ret = crt_rpc_register(GETATTR_OP, &GETATTR_FMT);
	if (ret) {
		IOF_LOG_ERROR("getattr registration failed with ret: %d", ret);
		return ret;
	}

	ret = crt_rpc_register(SHUTDOWN_OP, NULL);
	if (ret) {
		IOF_LOG_ERROR("shutdown registration failed with ret: %d", ret);
		return ret;
	}

	ret = crt_rpc_register(OPENDIR_OP, &OPENDIR_FMT);
	if (ret) {
		IOF_LOG_ERROR("Can not register opendir RPC, ret = %d", ret);
		return ret;
	}

	ret = crt_rpc_register(READDIR_OP, &READDIR_FMT);
	if (ret) {
		IOF_LOG_ERROR("Can not register readdir RPC, ret = %d", ret);
		return ret;
	}

	ret = crt_rpc_register(CLOSEDIR_OP, &CLOSEDIR_FMT);
	if (ret) {
		IOF_LOG_ERROR("Can not register closedir RPC, ret = %d", ret);
		return ret;
	}

	ret = crt_rpc_register(OPEN_OP, &OPEN_FMT);
	if (ret) {
		IOF_LOG_ERROR("Can not register open RPC, ret = %d", ret);
		return ret;
	}

	ret = crt_rpc_register(CLOSE_OP, &CLOSE_FMT);
	if (ret) {
		IOF_LOG_ERROR("Can not register close RPC, ret = %d", ret);
		return ret;
	}

	ret = crt_rpc_register(CREATE_OP, &CREATE_FMT);
	if (ret) {
		IOF_LOG_ERROR("Can not register create RPC, ret = %d", ret);
		return ret;
	}

	ret = crt_rpc_register(READ_OP, &READ_FMT);
	if (ret) {
		IOF_LOG_ERROR("Can not register read RPC, ret = %d", ret);
		return ret;
	}

	handle->cb = cb;

	return ret;
}

int iof_post_start(void *foo)
{
	struct iof_handle *iof_handle = (struct iof_handle *)foo;
	struct iof_psr_query *query = NULL;
	int ret;
	int i;
	int fs_num;
	char mount[IOF_NAME_LEN_MAX];
	char ctrl_path[IOF_NAME_LEN_MAX];
	char base_mount[IOF_NAME_LEN_MAX];
	struct cnss_plugin_cb *cb;
	struct iof_state *iof_state = NULL;
	struct fs_handle *fs_handle = NULL;
	struct iof_fs_info *tmp;
	crt_rpc_t *query_rpc = NULL;

	ret = IOF_SUCCESS;
	iof_state = iof_handle->state;
	cb = iof_handle->cb;


	/*Query PSR*/
	ret = ioc_get_projection_info(iof_state, &query, &query_rpc);
	if (ret || (query == NULL)) {
		IOF_LOG_ERROR("Query operation failed");
		return IOF_ERR_PROJECTION;
	}

	/*calculate number of projections*/
	fs_num = (query->query_list.iov_len)/sizeof(struct iof_fs_info);
	IOF_LOG_DEBUG("Number of filesystems projected: %d", fs_num);

	tmp = (struct iof_fs_info *) query->query_list.iov_buf;

	strncpy(base_mount, iof_state->cnss_prefix, IOF_NAME_LEN_MAX);
	for (i = 0; i < fs_num; i++) {
		char *base_name;

		if (tmp[i].mode == 0) {
			fs_handle = calloc(1, sizeof(struct fs_handle));
			if (!fs_handle)
				return IOF_ERR_NOMEM;
			fs_handle->iof_state = iof_state;
			IOF_LOG_INFO("Filesystem mode: Private");

		} else
			return IOF_NOT_SUPP;


		snprintf(ctrl_path, IOF_NAME_LEN_MAX, "/iof/PA/mount%d", i);
		IOF_LOG_DEBUG("Ctrl path is: %s%s", iof_state->cnss_prefix,
								ctrl_path);

		base_name = basename(tmp[i].mnt);

		IOF_LOG_DEBUG("Projected Mount %s", base_name);
		snprintf(mount, IOF_NAME_LEN_MAX, "%s/%s", base_mount,
								base_name);
		IOF_LOG_INFO("Mountpoint for this projection: %s", mount);

		/*Register the mount point with the control filesystem*/
		cb->register_ctrl_constant(ctrl_path, mount);

		fs_handle->my_fs_id = tmp[i].id;
		IOF_LOG_INFO("Filesystem ID %" PRIu64, tmp[i].id);
		if (cb->register_fuse_fs(cb->handle, &ops, mount, fs_handle)
								!= NULL)
			IOF_LOG_DEBUG("Fuse mount installed at: %s", mount);

	}

	ret = crt_req_decref(query_rpc);
	if (ret)
		IOF_LOG_ERROR("Could not decrement ref count on query rpc");
	return ret;
}

void iof_flush(void *handle)
{

	IOF_LOG_INFO("Called iof_flush");

}

static int shutdown_cb(const struct crt_cb_info *cb_info)
{
	int *complete;

	complete = (int *)cb_info->cci_arg;

	*complete = 1;

	return IOF_SUCCESS;
}

void iof_finish(void *handle)
{
	int ret;
	crt_rpc_t *shut_rpc;
	int complete;
	struct iof_handle *iof_handle = (struct iof_handle *)handle;
	struct iof_state *iof_state = iof_handle->state;

	/*send a detach RPC to IONSS*/
	ret = crt_req_create(iof_state->crt_ctx, iof_state->dest_ep,
			SHUTDOWN_OP, &shut_rpc);
	if (ret)
		IOF_LOG_ERROR("Could not create shutdown request ret = %d",
				ret);

	complete = 0;
	ret = crt_req_send(shut_rpc, shutdown_cb, &complete);
	if (ret)
		IOF_LOG_ERROR("shutdown RPC not sent");

	ret = iof_progress(iof_state->crt_ctx, 50, 6000, &complete);
	if (ret)
		IOF_LOG_ERROR("Could not progress shutdown RPC");
	ret = crt_context_destroy(iof_state->crt_ctx, 0);
	if (ret)
		IOF_LOG_ERROR("Could not destroy context");
	IOF_LOG_INFO("Called iof_finish with %p", handle);

	ret = crt_group_detach(iof_state->dest_group);
	if (ret)
		IOF_LOG_ERROR("crt_group_detach failed with ret = %d", ret);

	free(iof_state->cnss_prefix);
	free(iof_state);
	free(iof_handle);
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

	self.handle = calloc(1, sizeof(struct iof_handle));
	if (!self.handle)
		return IOF_ERR_NOMEM;
	*fns = &self;
	return IOF_SUCCESS;
}

