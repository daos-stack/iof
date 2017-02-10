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
	int complete;
	int err;
	int rc;
	struct stat *stat;
};

static int getattr_cb(const struct crt_cb_info *cb_info)
{
	struct getattr_cb_r *reply = NULL;
	crt_rpc_t *getattr_rpc;
	struct iof_getattr_out *out = NULL;

	getattr_rpc = cb_info->cci_rpc;
	reply = (struct getattr_cb_r *)cb_info->cci_arg;

	out = crt_reply_get(getattr_rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not get output");
		reply->complete = 1;
		return IOF_ERR_CART;
	}
	if (out->err == 0 && out->rc == 0)
		memcpy(reply->stat, out->stat.iov_buf, sizeof(struct stat));
	reply->err = out->err;
	reply->rc = out->rc;
	reply->complete = 1;
	return IOF_SUCCESS;
}

int ioc_getattr_name(const char *path, struct stat *stbuf)
{
	struct fs_handle *fs_handle = ioc_get_handle();
	struct iof_string_in *in;
	struct getattr_cb_r reply = {0};
	crt_rpc_t *rpc = NULL;
	int rc;

	IOF_LOG_DEBUG("Path: %s", path);

	rc = crt_req_create(fs_handle->crt_ctx, fs_handle->dest_ep,
			    FS_TO_OP(fs_handle, getattr), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u", rc);
		return -EIO;
	}

	in = crt_req_get(rpc);
	in->path = (crt_string_t)path;
	in->fs_id = fs_handle->fs_id;

	reply.complete = 0;
	reply.stat = stbuf;

	rc = crt_req_send(rpc, getattr_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send rpc, rc = %u", rc);
		return -EIO;
	}
	rc = ioc_cb_progress(fs_handle, &reply.complete);
	if (rc)
		return -rc;

	IOF_LOG_DEBUG("path %s rc %d",
		      path, reply.err == 0 ? -reply.rc : -EIO);

	return reply.err == 0 ? -reply.rc : -EIO;
}

#if IOF_USE_FUSE3
static int ioc_getattr_gah(struct stat *stbuf, struct fuse_file_info *fi)
{
	struct iof_file_handle *handle = (struct iof_file_handle *)fi->fh;
	struct fs_handle *fs_handle = handle->fs_handle;
	struct iof_gah_in *in;
	struct getattr_cb_r reply = {0};
	crt_rpc_t *rpc = NULL;
	int rc;

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(handle->gah));

	if (!handle->gah_valid) {
		/* If the server has reported that the GAH is invalid
		 * then do not send a RPC to close it
		 */
		return -EIO;
	}

	rc = crt_req_create(fs_handle->crt_ctx, fs_handle->dest_ep,
			    FS_TO_OP(fs_handle, getattr_gah), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u", rc);
		return -EIO;
	}

	in = crt_req_get(rpc);
	in->gah = handle->gah;

	reply.complete = 0;
	reply.stat = stbuf;

	rc = crt_req_send(rpc, getattr_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send rpc, rc = %u", rc);
		return -EIO;
	}
	rc = ioc_cb_progress(fs_handle, &reply.complete);
	if (rc)
		return -rc;

	IOF_LOG_DEBUG("rc %d", reply.err == 0 ? -reply.rc : -EIO);

	return reply.err == 0 ? -reply.rc : -EIO;
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
