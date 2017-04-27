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

struct opendir_cb_r {
	struct iof_dir_handle *dh;
	int complete;
	int err;
	int rc;
};

static int opendir_cb(const struct crt_cb_info *cb_info)
{
	struct opendir_cb_r *reply = NULL;
	struct iof_opendir_out *out = NULL;
	crt_rpc_t *rpc = cb_info->cci_rpc;

	reply = (struct opendir_cb_r *)cb_info->cci_arg;

	if (cb_info->cci_rc != 0) {
		/*
		 * Error handling.  On timeout return EAGAIN, all other errors
		 * return EIO.
		 *
		 * TODO: Handle target eviction here
		 */
		IOF_LOG_INFO("Bad RPC reply %d", cb_info->cci_rc);
		if (cb_info->cci_rc == -CER_TIMEDOUT)
			reply->rc = EAGAIN;
		else
			reply->rc = EIO;
		reply->complete = 1;
		return 0;
	}

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not get output");
		reply->rc = EIO;
		reply->complete = 1;
		return 0;
	}
	if (out->err == 0 && out->rc == 0) {
		reply->dh->gah = out->gah;
		reply->dh->gah_valid = 1;
		reply->dh->handle_valid = 1;
	}
	reply->err = out->err;
	reply->rc = out->rc;
	reply->complete = 1;
	return 0;
}

int ioc_opendir(const char *dir, struct fuse_file_info *fi)
{
	struct fs_handle *fs_handle = ioc_get_handle();
	struct iof_dir_handle *dir_handle;
	struct iof_string_in *in;
	struct opendir_cb_r reply = {0};
	size_t dir_len = strlen(dir) + 1;
	crt_rpc_t *rpc = NULL;
	int rc;

	dir_handle = calloc(1, dir_len + sizeof(*dir_handle));
	if (!dir_handle)
		return -ENOMEM;
	strncpy(dir_handle->name, dir, dir_len);

	dir_handle->fs_handle = fs_handle;

	IOF_LOG_INFO("dir %s handle %p", dir, dir_handle);

	STAT_ADD(fs_handle->stats, opendir);

	rc = crt_req_create(fs_handle->crt_ctx, fs_handle->dest_ep,
			    FS_TO_OP(fs_handle, opendir), &rpc);

	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u", rc);
		free(dir_handle);
		return -EIO;
	}

	in = crt_req_get(rpc);
	in->path = (crt_string_t)dir;
	in->fs_id = fs_handle->fs_id;

	reply.dh = dir_handle;

	rc = crt_req_send(rpc, opendir_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send rpc, rc = %u", rc);
		free(dir_handle);
		return -EIO;
	}

	rc = ioc_cb_progress(fs_handle, &reply.complete);
	if (rc) {
		free(dir_handle);
		return -rc;
	}

	if (reply.err == 0 && reply.rc == 0) {

		fi->fh = (uint64_t)dir_handle;

		IOF_LOG_INFO("Handle %p " GAH_PRINT_FULL_STR, dir_handle,
			     GAH_PRINT_FULL_VAL(dir_handle->gah));
	} else {
		free(dir_handle);
	}

	IOF_LOG_DEBUG("path %s rc %d",
		      dir, reply.err == 0 ? -reply.rc : -EIO);

	return reply.err == 0 ? -reply.rc : -EIO;
}

