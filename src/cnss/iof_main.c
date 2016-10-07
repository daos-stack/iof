/* Copyright (C) 2016 Intel Corporation
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
#ifdef IOF_USE_FUSE3
#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>
#else
#include <fuse/fuse.h>
#include <fuse/fuse_lowlevel.h>
#endif

#include "iof_common.h"
#include "cnss_plugin.h"
#include "iof.h"
#include "log.h"
#include "ctrl_fs.h"

struct query_reply {
	struct mcl_event event;
	int err_code;
	struct iof_psr_query *query;
};

struct getattr_cb_r {
	struct iof_getattr_out out;
	struct mcl_event event;
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

void get_my_time(struct timespec *ts)
{
#ifdef __APPLE__
	clock_serv_t cclock;
	mach_timespec_t mts;

	host_get_clock_service(mach_host_self(), SYSTEM_CLOCK, &cclock);
	clock_get_time(cclock, &mts);
	mach_port_deallocate(mach_task_self(), cclock);
	ts->tv_sec = mts.tv_sec;
	ts->tv_nsec = mts.tv_nsec;
#else
	clock_gettime(CLOCK_MONOTONIC, ts);
#endif

}
/*
 * using timeout here is only a temporary fix and
 * will be removed after migration to CaRT
 *
 */
int iof_progress(struct mcl_context *mcl_context, struct mcl_event *cb_event,
		int iof_timeout)
{
	int ret;
	struct timespec start, end;
	double time_spent;
	double timeout;

	timeout = (double)iof_timeout/1000;

	ret = 0;
	get_my_time(&start);
	while (!mcl_event_test(cb_event)) {
		mcl_progress(mcl_context, NULL);
		get_my_time(&end);
		time_spent = (double) end.tv_sec - start.tv_sec;
		if ((timeout - time_spent) <= 0) {
			ret = 1;
			break;
		}
	}

	return ret;
}

static hg_return_t getattr_cb(const struct hg_cb_info *info)
{
	struct getattr_cb_r *reply = NULL;
	int ret;

	IOF_LOG_DEBUG("Callback invoked");
	reply = info->arg;
	ret = HG_Get_output(info->info.forward.handle, &reply->out);
	if (ret)
		IOF_LOG_ERROR("could not retrieve getattr output");
	mcl_event_set(&reply->event);
	return ret;
}

static int iof_getattr(const char *path, struct stat *stbuf)
{
	struct fuse_context *context;
	uint64_t ret;
	hg_handle_t rpc_handle;
	struct iof_string_in in;
	struct getattr_cb_r reply = {0};
	struct fs_handle *fs_handle;
	struct mcl_context *mcl_context = NULL;
	struct iof_state *iof_state = NULL;

	/*retrieve handle*/
	context = fuse_get_context();
	fs_handle = (struct fs_handle *)context->private_data;
	mcl_context = fs_handle->iof_state->context;
	iof_state = fs_handle->iof_state;
	IOF_LOG_DEBUG("Path: %s", path);

	in.name = path;
	in.my_fs_id = (uint64_t)fs_handle->my_fs_id;
	mcl_event_clear(&reply.event);

	ret = HG_Create(mcl_context->context, iof_state->psr_addr,
			iof_state->getattr_id, &rpc_handle);
	if (ret) {
		IOF_LOG_ERROR("Getattr: Handle not created");
		return -ENOMEM;
	}
	ret = HG_Forward(rpc_handle, getattr_cb, &reply, &in);
	if (ret)
		IOF_LOG_ERROR("Getattr: RPC not sent");

	ret = iof_progress(mcl_context, &reply.event, 6000);
	if (ret) {
		IOF_LOG_ERROR("Getattr: exiting fuse loop");
		fuse_session_exit(fuse_get_session(context->fuse));
		return -EINTR;
	}
	memcpy(stbuf, &reply.out.stbuf, sizeof(struct stat));
	ret = HG_Free_output(rpc_handle, &reply.out);
	ret = HG_Destroy(rpc_handle);
	if (ret)
		IOF_LOG_ERROR("Getattr: could not destroy handle");

	return reply.out.err;

}

#ifdef __APPLE__

static int iof_setxattr(const char *path,  const char *name, const char *value,
			size_t size, int options, uint32_t position)
#else
static int iof_setxattr(const char *path, const char *name, const char *value,
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


static int iof_readdir(const char *dir_name, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi
#ifdef IOF_USE_FUSE3
	, enum fuse_readdir_flags flags
#endif
	)
{
	IOF_LOG_INFO("Readdir: Not currently implemented");
	return -EINVAL;
}

static struct fuse_operations ops = {
	.getattr = iof_getattr,
	.readdir = iof_readdir,
	.setxattr = iof_setxattr,
};

static hg_return_t query_callback(const struct hg_cb_info *info)
{
	struct query_reply *reply;
	int ret;

	reply = info->arg;
	ret = HG_Get_output(info->info.forward.handle, reply->query);
	if (ret != HG_SUCCESS)
		IOF_LOG_ERROR("Cant unpack output of RPC");
	/* Decrement the ref count.  Safe to do here because no data
	 * is allocated but the handle can't be destroyed until the
	 * reference count is 0.
	 */
	ret = HG_Free_output(info->info.forward.handle, reply->query);
	if (ret != HG_SUCCESS)
		IOF_LOG_ERROR("Error freeing output");
	reply->err_code = ret;
	mcl_event_set(&reply->event);
	return IOF_SUCCESS;
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
		HG_Destroy(handle); /* Ignore errors */
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
	struct iof_handle *handle = (struct iof_handle *)foo;
	struct iof_state *iof_state;
	char *prefix;

	if (!handle->state) {
		handle->state = calloc(1, sizeof(struct iof_state));
		if (!handle->state)
			return IOF_ERR_NOMEM;
	}
	/*initialize iof state*/
	iof_state = handle->state;
	iof_state->mcl_state = state;
	iof_state->context = mcl_get_context(state);
	prefix = getenv("CNSS_PREFIX");
	if (snprintf(iof_state->cnss_prefix, IOF_PREFIX_MAX, prefix) >
			IOF_PREFIX_MAX)
		return IOF_ERR_OVERFLOW;
	iof_state->projection_query = HG_Register_name(state->hg_class,
							"Projection_query",
							NULL,
							iof_query_out_proc_cb,
							iof_query_handler);
	IOF_LOG_INFO("Id registered on CNSS: %d", iof_state->projection_query);
	iof_state->getattr_id = HG_Register_name(state->hg_class, "getattr",
				iof_string_in_proc_cb,
				iof_getattr_out_proc_cb,
				iof_getattr_handler);

	handle->cb = cb;

	return IOF_SUCCESS;
}

int iof_post_start(void *foo, struct mcl_set *set)
{
	struct iof_handle *iof_handle = (struct iof_handle *)foo;
	struct iof_psr_query query = {0};
	int ret;
	int i;
	char mount[IOF_NAME_LEN_MAX];
	char ctrl_path[IOF_NAME_LEN_MAX];
	char base_mount[IOF_NAME_LEN_MAX];
	struct cnss_plugin_cb *cb;
	struct iof_state *iof_state = NULL;
	struct fs_handle *fs_handle = NULL;

	ret = IOF_SUCCESS;
	iof_state = iof_handle->state;
	cb = iof_handle->cb;

	ret = mcl_lookup(set, set->psr_rank, iof_state->context,
			 &iof_state->psr_addr);
	if (ret != MCL_SUCCESS) {
		IOF_LOG_ERROR("PSR Address lookup failed");
		return IOF_ERR_MCL;
	}

	/*Query PSR*/
	ret = ioc_get_projection_info(iof_state->context, iof_state->psr_addr,
				&query, iof_state->projection_query);
	if (ret != 0) {
		IOF_LOG_ERROR("Query operation failed");
		return IOF_ERR_PROJECTION;
	}

	for (i = 0; i < query.num; i++) {

		struct iof_fs_info *tmp = &query.list[i];

		snprintf(base_mount, IOF_PREFIX_MAX, iof_state->cnss_prefix);
		if (tmp->mode == 0) {
			fs_handle = calloc(1, sizeof(struct fs_handle));
			if (!fs_handle)
				return IOF_ERR_NOMEM;
			fs_handle->iof_state = iof_state;
			IOF_LOG_INFO("Filesystem mode: Private");

		} else
			return IOF_NOT_SUPP;


		if (mkdir(base_mount, 0755) && errno != EEXIST) {
			IOF_LOG_ERROR("Could not create base mount %s",
					base_mount);
			continue;
		}
		snprintf(ctrl_path, IOF_NAME_LEN_MAX, "/iof/PA/mount%d", i);
		IOF_LOG_DEBUG("Ctrl path is: %s%s", iof_state->cnss_prefix,
								ctrl_path);

		IOF_LOG_DEBUG("Projected Mount %s", tmp->mnt);
		snprintf(mount, IOF_NAME_LEN_MAX, "%s/%s\n", base_mount,
								tmp->mnt);
		IOF_LOG_INFO("Mountpoint for this projection: %s", mount);

		/*Register the mount point with the control filesystem*/
		cb->register_ctrl_constant(ctrl_path, mount);

		fs_handle->my_fs_id = tmp->id;
		IOF_LOG_INFO("Filesystem ID %" PRIu64, tmp->id);
		if (cb->register_fuse_fs(cb->handle, &ops, mount, fs_handle)
								!= NULL)
			IOF_LOG_DEBUG("Fuse mount installed at: %s", mount);

	}

	free(query.list);
	return ret;
}

void iof_flush(void *handle)
{

	IOF_LOG_INFO("Called iof_flush");

}

void iof_finish(void *handle)
{
	struct iof_handle *iof_handle = (struct iof_handle *)handle;
	struct iof_state *iof_state = iof_handle->state;

	IOF_LOG_INFO("Called iof_finish with %p", handle);
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

