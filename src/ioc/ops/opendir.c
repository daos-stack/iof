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

#include "iof_common.h"
#include "ioc.h"
#include "log.h"
#include "ios_gah.h"

struct opendir_cb_r {
	struct iof_dir_handle *dh;
	struct iof_tracker tracker;
	int err;
	int rc;
};

static void
opendir_cb(const struct crt_cb_info *cb_info)
{
	struct opendir_cb_r *reply = cb_info->cci_arg;
	struct iof_opendir_out *out = crt_reply_get(cb_info->cci_rpc);

	if (cb_info->cci_rc != 0) {
		/*
		 * Error handling.  On timeout return EAGAIN, all other errors
		 * return EIO.
		 *
		 * TODO: Handle target eviction here
		 */
		IOF_LOG_INFO("Bad RPC reply %d", cb_info->cci_rc);
		if (cb_info->cci_rc == -DER_TIMEDOUT)
			reply->rc = EAGAIN;
		else
			reply->rc = EIO;
		iof_tracker_signal(&reply->tracker);
		return;
	}

	if (out->err == 0 && out->rc == 0) {
		reply->dh->gah = out->gah;
		reply->dh->gah_valid = 1;
		reply->dh->handle_valid = 1;
	}
	reply->err = out->err;
	reply->rc = out->rc;
	iof_tracker_signal(&reply->tracker);
}

int ioc_opendir(const char *dir, struct fuse_file_info *fi)
{
	struct iof_projection_info *fs_handle = ioc_get_handle();
	struct iof_dir_handle *dir_handle;
	struct iof_string_in *in;
	struct opendir_cb_r reply = {0};
	int rc;

	STAT_ADD(fs_handle->stats, opendir);

	if (FS_IS_OFFLINE(fs_handle))
		return -fs_handle->offline_reason;

	dir_handle = iof_pool_acquire(fs_handle->dh);
	if (!dir_handle)
		return -ENOMEM;

	dir_handle->fs_handle = fs_handle;
	dir_handle->ep = fs_handle->proj.grp->psr_ep;

	IOF_LOG_INFO("dir %s handle %p", dir, dir_handle);

	iof_tracker_init(&reply.tracker, 1);
	in = crt_req_get(dir_handle->open_rpc);
	in->path = (d_string_t)dir;
	in->fs_id = fs_handle->fs_id;

	reply.dh = dir_handle;

	rc = crt_req_send(dir_handle->open_rpc, opendir_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send rpc, rc = %u", rc);
		iof_pool_release(fs_handle->dh, dir_handle);
		return -EIO;
	}
	dir_handle->open_rpc = NULL;
	dir_handle->name = strdup(dir);
	iof_pool_restock(fs_handle->dh);

	iof_fs_wait(&fs_handle->proj, &reply.tracker);

	if (reply.err == 0 && reply.rc == 0) {

		fi->fh = (uint64_t)dir_handle;

		IOF_LOG_INFO("Handle %p " GAH_PRINT_FULL_STR, dir_handle,
			     GAH_PRINT_FULL_VAL(dir_handle->gah));
	} else {
		iof_pool_release(fs_handle->dh, dir_handle);
	}

	IOF_LOG_DEBUG("path %s rc %d",
		      dir, reply.err == 0 ? -reply.rc : -EIO);

	return reply.err == 0 ? -reply.rc : -EIO;
}
