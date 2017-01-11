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
#include <fuse3/fuse_lowlevel.h>
#else
#include <fuse/fuse.h>
#include <fuse/fuse_lowlevel.h>
#endif

#include "iof_common.h"
#include "iof.h"
#include "log.h"
#include "ios_gah.h"

struct create_cb_r {
	struct iof_file_handle *fh;
	int complete;
	int err;
	int rc;
};

static int create_cb(const struct crt_cb_info *cb_info)
{
	struct create_cb_r *reply;
	struct iof_open_out *out;
	crt_rpc_t *rpc = cb_info->cci_rpc;

	reply = (struct create_cb_r *)cb_info->cci_arg;

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

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not get output");
		reply->complete = 1;
		return 0;
	}
	if (out->err == 0 && out->rc == 0)
		memcpy(&reply->fh->gah, out->gah.iov_buf,
		       sizeof(struct ios_gah));
	reply->err = out->err;
	reply->rc = out->rc;
	reply->complete = 1;
	return 0;
}

int ioc_create(const char *file, mode_t mode, struct fuse_file_info *fi)
{
	struct fuse_context *context;
	struct iof_file_handle *handle;
	uint64_t ret;
	struct iof_create_in *in = NULL;
	struct create_cb_r reply = {0};
	struct fs_handle *fs_handle;
	struct iof_state *iof_state = NULL;
	crt_rpc_t *rpc = NULL;
	int rc;

	handle = ioc_fh_new(file);
	if (!handle)
		return -ENOMEM;

	fi->fh = (uint64_t)handle;

	IOF_LOG_INFO("file %s handle %p", file, handle);

	context = fuse_get_context();
	fs_handle = (struct fs_handle *)context->private_data;
	iof_state = fs_handle->iof_state;
	if (!iof_state) {
		IOF_LOG_ERROR("Could not retrieve iof state");
		return -EIO;
	}

	ret = crt_req_create(iof_state->crt_ctx, iof_state->dest_ep, CREATE_OP,
			     &rpc);
	if (ret || !rpc) {
		IOF_LOG_ERROR("Could not create request, ret = %lu",
			      ret);
		return -EIO;
	}

	in = crt_req_get(rpc);
	in->path = (crt_string_t)file;
	in->mode = mode;
	in->my_fs_id = (uint64_t)fs_handle->my_fs_id;

	reply.fh = handle;
	reply.complete = 0;

	ret = crt_req_send(rpc, create_cb, &reply);
	if (ret) {
		IOF_LOG_ERROR("Could not send rpc, ret = %lu", ret);
		return -EIO;
	}
	rc = ioc_cb_progress(iof_state->crt_ctx, context, &reply.complete);
	if (rc) {
		free(handle);
		return -rc;
	}

	if (reply.err == 0 && reply.rc == 0) {
		char *d = ios_gah_to_str(&handle->gah);

		IOF_LOG_INFO("Dah %s", d);
		free(d);
	}

	rc = reply.err == 0 ? -reply.rc : -EIO;

	if (rc != 0)
		free(handle);

	IOF_LOG_DEBUG("path %s rc %d",
		      file, rc);

	return rc;
}

