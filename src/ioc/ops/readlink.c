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

struct readlink_cb_r {
	struct iof_string_out *out;
	crt_rpc_t *rpc;
	struct iof_tracker tracker;
	int err;
	int rc;
};

static void
ioc_readlink_cb(const struct crt_cb_info *cb_info)
{
	struct readlink_cb_r *reply = cb_info->cci_arg;
	struct iof_string_out *out = crt_reply_get(cb_info->cci_rpc);
	int rc;

	if (cb_info->cci_rc != 0) {
		/*
		 * Error handling.  Return EIO on any error
		 */
		IOF_LOG_INFO("Bad RPC reply %d", cb_info->cci_rc);
		reply->err = EIO;
		iof_tracker_signal(&reply->tracker);
		return;
	}

	if (out->err) {
		IOF_LOG_ERROR("Error from target %d", out->err);

		reply->err = EIO;
		iof_tracker_signal(&reply->tracker);
		return;
	}

	if (out->rc) {
		reply->rc = out->rc;
		iof_tracker_signal(&reply->tracker);
		return;
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
}

int ioc_readlink(const char *link, char *target, size_t len)
{
	struct iof_projection_info *fs_handle = ioc_get_handle();
	struct iof_string_in *in;
	struct readlink_cb_r reply = {0};
	crt_rpc_t *rpc = NULL;
	int rc;

	STAT_ADD(fs_handle->stats, readlink);

	if (FS_IS_OFFLINE(fs_handle))
		return -fs_handle->offline_reason;

	IOF_LOG_INFO("link %s", link);

	rc = crt_req_create(fs_handle->proj.crt_ctx,
			    &fs_handle->proj.grp->psr_ep,
			    FS_TO_OP(fs_handle, readlink), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u",
			      rc);
		return -EIO;
	}

	iof_tracker_init(&reply.tracker, 1);
	in = crt_req_get(rpc);
	in->path = (d_string_t)link;
	in->fs_id = fs_handle->fs_id;

	rc = crt_req_send(rpc, ioc_readlink_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send rpc, rc = %u", rc);
		return -EIO;
	}
	iof_fs_wait(&fs_handle->proj, &reply.tracker);

	if (!reply.rpc)
		return -EIO;

	IOF_LOG_DEBUG("%d %d %p %s",
		      reply.err,
		      reply.rc,
		      reply.out->path,
		      reply.out->path);

	if (reply.out->path) {
		IOF_LOG_DEBUG("Path is %s", reply.out->path);
		strncpy(target, reply.out->path, len);
	} else {
		reply.err = EIO;
	}

	rc = crt_req_decref(reply.rpc);
	if (rc)
		IOF_LOG_ERROR("decref returned %d", rc);

	IOF_LOG_DEBUG("link %s rc %d", link, IOC_STATUS_TO_RC(&reply));

	return IOC_STATUS_TO_RC(&reply);
}

static void
readlink_ll_cb(const struct crt_cb_info *cb_info)
{
	fuse_req_t req = cb_info->cci_arg;
	struct iof_string_out *out = crt_reply_get(cb_info->cci_rpc);

	if (cb_info->cci_rc != 0) {
		fuse_reply_err(req, EIO);
		return;
	}

	if (out->rc) {
		fuse_reply_err(req, out->rc);
		return;
	}

	if (out->err || !out->path) {
		fuse_reply_err(req, EIO);
		return;
	}

	fuse_reply_readlink(req, out->path);
}

void
ioc_ll_readlink(fuse_req_t req, fuse_ino_t ino)
{
	struct iof_projection_info *fs_handle = fuse_req_userdata(req);
	struct iof_gah_in *in;
	crt_rpc_t *rpc = NULL;
	int rc;
	int ret;

	STAT_ADD(fs_handle->stats, readlink);

	if (FS_IS_OFFLINE(fs_handle)) {
		ret = fs_handle->offline_reason;
		goto out_err;
	}

	rc = crt_req_create(fs_handle->proj.crt_ctx,
			    &fs_handle->proj.grp->psr_ep,
			    FS_TO_OP(fs_handle, readlink_ll), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u",
			      rc);
		ret = EIO;
		goto out_err;
	}

	in = crt_req_get(rpc);
	in->gah = fs_handle->gah;

	/* Find the GAH of the parent */
	rc = find_gah(fs_handle, ino, &in->gah);
	if (rc != 0) {
		ret = ENOENT;
		goto out_err;
	}

	rc = crt_req_send(rpc, readlink_ll_cb, req);
	if (rc) {
		IOF_LOG_ERROR("Could not send rpc, rc = %u", rc);
		ret = EIO;
		goto out_err;
	}

	return;

out_err:
	fuse_reply_err(req, ret);
}
