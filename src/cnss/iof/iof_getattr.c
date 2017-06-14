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

#ifdef IOF_USE_FUSE3
#include <fuse3/fuse.h>
#else
#include <fuse/fuse.h>
#endif
#include "iof_common.h"
#include "iof.h"
#include "log.h"

struct getattr_cb_r {
	struct iof_tracker tracker;
	int err;
	int rc;
	struct stat *stat;
};

static int getattr_cb(const struct crt_cb_info *cb_info)
{
	struct getattr_cb_r *reply = cb_info->cci_arg;
	struct iof_getattr_out *out = crt_reply_get(cb_info->cci_rpc);

	if (IOC_HOST_IS_DOWN(cb_info)) {
		reply->err = EHOSTDOWN;
		iof_tracker_signal(&reply->tracker);
		return 0;
	}

	if (cb_info->cci_rc != 0) {
		reply->err = EIO;
		iof_tracker_signal(&reply->tracker);
		return 0;
	}

	reply->rc = out->rc;

	if (out->err)
		reply->err = EIO;

	if (IOC_STATUS_TO_RC(reply) == 0)
		memcpy(reply->stat, out->stat.iov_buf, sizeof(struct stat));

	iof_tracker_signal(&reply->tracker);
	return 0;
}

int ioc_getattr_name(const char *path, struct stat *stbuf)
{
	struct iof_projection_info *fs_handle = ioc_get_handle();
	struct iof_string_in *in;
	struct getattr_cb_r reply = {0};
	crt_rpc_t *rpc = NULL;
	int rc;

	IOF_LOG_DEBUG("Path: %s", path);

	STAT_ADD(fs_handle->stats, getattr);

	if (FS_IS_OFFLINE(fs_handle))
		return -fs_handle->offline_reason;

	rc = crt_req_create(fs_handle->proj.crt_ctx, fs_handle->dest_ep,
			    FS_TO_OP(fs_handle, getattr), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u", rc);
		return -EIO;
	}

	in = crt_req_get(rpc);
	in->path = (crt_string_t)path;
	in->fs_id = fs_handle->fs_id;

	iof_tracker_init(&reply.tracker, 1);
	reply.stat = stbuf;

	rc = crt_req_send(rpc, getattr_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send rpc, rc = %u", rc);
		return -EIO;
	}
	iof_fs_wait(&fs_handle->proj, &reply.tracker);

	if (reply.err == EHOSTDOWN)
		ioc_mark_ep_offline(fs_handle, &rpc->cr_ep);

	IOF_LOG_DEBUG("path %s rc %d", path, IOC_STATUS_TO_RC(&reply));

	return IOC_STATUS_TO_RC(&reply);
}

#if IOF_USE_FUSE3
static int ioc_getattr_gah(struct stat *stbuf, struct fuse_file_info *fi)
{
	struct iof_file_handle *handle = (struct iof_file_handle *)fi->fh;
	struct iof_projection_info *fs_handle = handle->fs_handle;
	struct iof_gah_in *in;
	struct getattr_cb_r reply = {0};
	crt_rpc_t *rpc = NULL;
	int rc;

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(handle->common.gah));

	STAT_ADD(fs_handle->stats, getfattr);

	if (FS_IS_OFFLINE(fs_handle))
		return -fs_handle->offline_reason;

	if (!handle->common.gah_valid) {
		/* If the server has reported that the GAH is invalid
		 * then do not send a RPC to close it
		 */
		return -EIO;
	}

	rc = crt_req_create(fs_handle->proj.crt_ctx, handle->common.ep,
			    FS_TO_OP(fs_handle, getattr_gah), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u", rc);
		return -EIO;
	}

	in = crt_req_get(rpc);
	in->gah = handle->common.gah;

	iof_tracker_init(&reply.tracker, 1);
	reply.stat = stbuf;

	rc = crt_req_send(rpc, getattr_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send rpc, rc = %u", rc);
		return -EIO;
	}
	iof_fs_wait(&fs_handle->proj, &reply.tracker);

	if (reply.err == EHOSTDOWN)
		ioc_mark_ep_offline(fs_handle, &rpc->cr_ep);

	/* Cache the inode number */
	if (IOC_STATUS_TO_RC(&reply) == 0)
		handle->inode_no = stbuf->st_ino;

	IOF_LOG_DEBUG(GAH_PRINT_STR " rc %d", GAH_PRINT_VAL(handle->common.gah),
		      IOC_STATUS_TO_RC(&reply));

	return IOC_STATUS_TO_RC(&reply);
}

int ioc_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	if (fi)
		return ioc_getattr_gah(stbuf, fi);

	if (!path)
		return -EIO;
	return ioc_getattr_name(path, stbuf);
}
#endif
