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

struct statfs_cb_r {
	struct iof_data_out *out;
	crt_rpc_t *rpc;
	struct iof_tracker tracker;
	int err;
	int rc;
};

static int statfs_cb(const struct crt_cb_info *cb_info)
{
	struct statfs_cb_r *reply = cb_info->cci_arg;
	struct iof_data_out *out = crt_reply_get(cb_info->cci_rpc);
	int rc;

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
		iof_tracker_signal(&reply->tracker);
		return 0;
	}

	if (out->err) {
		IOF_LOG_ERROR("Error from target %d", out->err);

		reply->err = EIO;
		iof_tracker_signal(&reply->tracker);
		return 0;
	}

	if (out->rc) {
		reply->rc = out->rc;
		iof_tracker_signal(&reply->tracker);
		return 0;
	}

	rc = crt_req_addref(cb_info->cci_rpc);
	if (rc) {
		IOF_LOG_ERROR("could not take reference on query RPC, rc = %d",
			      rc);
		reply->err = EIO;
	} else {
		reply->out = out;
		reply->rpc = cb_info->cci_rpc;
	}

	iof_tracker_signal(&reply->tracker);
	return 0;
}

int ioc_statfs(const char *path, struct statvfs *stat)
{
	struct iof_projection_info *fs_handle = ioc_get_handle();
	struct iof_string_in *in;
	struct statfs_cb_r reply = {0};
	crt_rpc_t *rpc = NULL;
	int rc;

	IOF_LOG_INFO("path %s", path);

	STAT_ADD(fs_handle->stats, statfs);

	if (FS_IS_OFFLINE(fs_handle))
		return -fs_handle->offline_reason;

	rc = crt_req_create(fs_handle->proj.crt_ctx, fs_handle->dest_ep,
			    FS_TO_OP(fs_handle, statfs), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u",
			      rc);
		return -EIO;
	}

	iof_tracker_init(&reply.tracker, 1);
	in = crt_req_get(rpc);
	in->path = (crt_string_t)path;
	in->fs_id = fs_handle->fs_id;

	rc = crt_req_send(rpc, statfs_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send rpc, rc = %u", rc);
		return -EIO;
	}

	iof_fs_wait(&fs_handle->proj, &reply.tracker);

	IOF_LOG_INFO("path %s rc %d", path, IOC_STATUS_TO_RC(reply));

	if (!reply.err && !reply.rc)
		memcpy(stat, reply.out->data.iov_buf, sizeof(*stat));

	rc = crt_req_decref(reply.rpc);
	if (rc)
		IOF_LOG_ERROR("decref returned %d", rc);

	return IOC_STATUS_TO_RC(reply);
}
