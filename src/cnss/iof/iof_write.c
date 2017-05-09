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
#include "ios_gah.h"

struct write_cb_r {
	struct iof_file_handle *handle;
	size_t len;
	int complete;
	int err;
	int rc;
};

#define BULK_THRESHOLD 64

static int write_cb(const struct crt_cb_info *cb_info)
{
	struct write_cb_r *reply;
	struct iof_write_out *out;

	reply = (struct write_cb_r *)cb_info->cci_arg;

	if (cb_info->cci_rc != 0) {
		/*
		 * Error handling.  On timeout return EAGAIN, all other errors
		 * return EIO.
		 *
		 * TODO: Handle target eviction here
		 */
		IOF_LOG_INFO("Bad RPC reply %d", cb_info->cci_rc);
		if (cb_info->cci_rc == -CER_TIMEDOUT)
			reply->err = EAGAIN;
		else
			reply->err = EIO;
		reply->complete = 1;
		return 0;
	}

	out = crt_reply_get(cb_info->cci_rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not get reply");
		reply->err = EIO;
		reply->complete = 1;
		return 0;
	}

	if (out->err) {
		/* Convert the error types, out->err is a IOF error code
		 * so translate it to a errno we can pass back to FUSE.
		 */
		IOF_LOG_ERROR("Error from target %d", out->err);

		reply->err = EIO;
		if (out->err == IOF_GAH_INVALID)
			reply->handle->gah_valid = 0;

		if (out->err == IOF_ERR_NOMEM)
			reply->err = EAGAIN;

		reply->complete = 1;
		return 0;
	}

	reply->len = out->len;
	reply->rc = out->rc;
	reply->complete = 1;
	return 0;
}

int ioc_write_direct(const char *buff, size_t len, off_t position,
		     struct iof_file_handle *handle)
{
	struct fs_handle *fs_handle = handle->fs_handle;
	struct iof_write_in *in;
	struct write_cb_r reply = {0};

	crt_rpc_t *rpc = NULL;
	int rc;

	IOF_LOG_INFO("path %s handle %p", handle->name, handle);

	rc = crt_req_create(fs_handle->crt_ctx, fs_handle->dest_ep,
			    FS_TO_OP(fs_handle, write_direct), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u",
			      rc);
		return -EIO;
	}

	in = crt_req_get(rpc);
	in->gah = handle->gah;
	crt_iov_set(&in->data, (void *)buff, len);
	in->base = position;

	reply.handle = handle;

	rc = crt_req_send(rpc, write_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send open rpc, rc = %u", rc);
		return -EIO;
	}

	rc = ioc_cb_progress(fs_handle, &reply.complete);
	if (rc)
		return -rc;

	if (reply.err != 0)
		return -reply.err;

	if (reply.rc != 0)
		return -reply.rc;

	return reply.len;
}

int ioc_write_bulk(const char *buff, size_t len, off_t position,
		   struct iof_file_handle *handle)
{
	struct fs_handle *fs_handle = handle->fs_handle;
	struct iof_write_bulk *in;
	crt_bulk_t bulk;
	struct write_cb_r reply = {0};

	crt_sg_list_t sgl = {0};
	crt_iov_t iov = {0};
	crt_rpc_t *rpc = NULL;
	int rc;

	rc = crt_req_create(fs_handle->crt_ctx, fs_handle->dest_ep,
			    FS_TO_OP(fs_handle, write_bulk), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u",
			      rc);
		return -EIO;
	}

	in = crt_req_get(rpc);

	in->gah = handle->gah;

	iov.iov_len = len;
	iov.iov_buf_len = len;
	iov.iov_buf = (void *)buff;
	sgl.sg_iovs = &iov;
	sgl.sg_nr.num = 1;

	rc = crt_bulk_create(fs_handle->crt_ctx, &sgl, CRT_BULK_RO, &in->bulk);
	if (rc) {
		IOF_LOG_ERROR("Failed to make local bulk handle %d", rc);
		return -EIO;
	}

	in->base = position;

	bulk = in->bulk;

	reply.handle = handle;

	rc = crt_req_send(rpc, write_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send open rpc, rc = %u", rc);
		return -EIO;
	}

	rc = ioc_cb_progress(fs_handle, &reply.complete);
	if (rc)
		return -rc;

	rc = crt_bulk_free(bulk);
	if (rc)
		return -EIO;

	if (reply.err != 0)
		return -reply.err;

	if (reply.rc != 0)
		return -reply.rc;

	return reply.len;
}

int ioc_write(const char *file, const char *buff, size_t len, off_t position,
	      struct fuse_file_info *fi)
{
	struct iof_file_handle *handle = (struct iof_file_handle *)fi->fh;
	int rc;

	IOF_LOG_INFO("%#zx-%#zx " GAH_PRINT_STR, position, position + len - 1,
		     GAH_PRINT_VAL(handle->gah));

	STAT_ADD(handle->fs_handle->stats, write);

	if (!IOF_IS_WRITEABLE(handle->fs_handle->flags)) {
		IOF_LOG_INFO("Attempt to modify Read-Only File System");
		return -EROFS;
	}

	if (!handle->gah_valid) {
		/* If the server has reported that the GAH is invalid
		 * then do not send a RPC to close it
		 */
		return -EIO;
	}

	if (len >= BULK_THRESHOLD)
		rc = ioc_write_bulk(buff, len, position, handle);
	else
		rc = ioc_write_direct(buff, len, position, handle);

	if (rc > 0)
		STAT_ADD_COUNT(handle->fs_handle->stats, write_bytes, rc);

	return rc;
}
