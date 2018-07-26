/* Copyright (C) 2017-2018 Intel Corporation
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

/*
 * TODO: Merge this with getattr_ll_cb() when this is updated to use requests
 */
static void
attr_ll_cb(const struct crt_cb_info *cb_info)
{
	struct iof_attr_out	*out = crt_reply_get(cb_info->cci_rpc);
	fuse_req_t		req = cb_info->cci_arg;
	int			ret = EIO;

	if (cb_info->cci_rc == -DER_NOMEM)
		D_GOTO(out_err, ret = ENOMEM);

	if (cb_info->cci_rc != 0) {
		IOF_TRACE_INFO(req, "cci_rc is %d", cb_info->cci_rc);
		goto out_err;
	}

	IOF_TRACE_DEBUG(req, "rc %d err %d", out->rc, out->err);

	if (out->err)
		D_GOTO(out_err, ret = EIO);
	if (out->rc)
		D_GOTO(out_err, ret = out->rc);

	IOF_FUSE_REPLY_ATTR(req, &out->stat);
	return;

out_err:
	IOF_FUSE_REPLY_ERR(req, ret);
}

void
ioc_ll_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr, int to_set,
	       struct fuse_file_info *fi)
{
	struct iof_projection_info	*fs_handle = fuse_req_userdata(req);
	struct iof_file_handle		*handle = NULL;
	struct iof_setattr_in		*in;
	crt_rpc_t			*rpc = NULL;
	int rc;

	if (fi)
		handle = (void *)fi->fh;

	IOF_TRACE_UP(req, handle ? handle : (void *)fs_handle, "getattr");

	STAT_ADD(fs_handle->stats, setattr);

	if (FS_IS_OFFLINE(fs_handle))
		D_GOTO(err, rc = fs_handle->offline_reason);

	rc = crt_req_create(fs_handle->proj.crt_ctx,
			    &fs_handle->proj.grp->psr_ep,
			    FS_TO_OP(fs_handle, setattr), &rpc);
	if (rc || !rpc) {
		IOF_TRACE_ERROR(req, "Could not create request, rc = %d", rc);
		D_GOTO(err, rc = EIO);
	}

	in = crt_req_get(rpc);

	if (handle) {
		if (!F_GAH_IS_VALID(handle))
			D_GOTO(err, rc = EIO);

		D_MUTEX_LOCK(&fs_handle->gah_lock);
		in->gah = handle->common.gah;
		D_MUTEX_UNLOCK(&fs_handle->gah_lock);
	} else {
		/* Find the GAH of the inode */
		rc = find_gah(fs_handle, ino, &in->gah);
		if (rc != 0)
			D_GOTO(err, 0);
	}

	in->to_set = to_set;

	memcpy(&in->stat, attr, sizeof(*attr));

	rc = crt_req_send(rpc, attr_ll_cb, req);
	if (rc) {
		IOF_TRACE_ERROR(req, "Could not send rpc, rc = %d", rc);
		D_GOTO(err, rc = EIO);
	}

	return;

err:
	IOF_FUSE_REPLY_ERR(req, rc);

	if (rpc)
		crt_req_decref(rpc);
}
