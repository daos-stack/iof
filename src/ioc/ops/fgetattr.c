/* Copyright (C) 2016-2018 Intel Corporation
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

#define REQ_NAME request
#define POOL_NAME fgh_pool
#define TYPE_NAME common_req
#define RESTOCK_ON_SEND
#include "ioc_ops.h"

#define STAT_KEY getattr

static void getattr_ll_cb(struct ioc_request *request)
{
	struct iof_attr_out *out = crt_reply_get(request->rpc);
	struct TYPE_NAME *desc;

	IOC_REQUEST_RESOLVE(request, out);

	if (request->rc == 0)
		IOF_FUSE_REPLY_ATTR(request->req, &out->stat);
	else
		IOF_FUSE_REPLY_ERR(request->req, request->rc);

	desc = CONTAINER(request);
	iof_pool_release(desc->request.fsh->fgh_pool, desc);
}

static const struct ioc_request_api api = {
	.on_send	= post_send,
	.on_result	= getattr_ll_cb,
	.on_evict	= ioc_simple_resend
};

void
ioc_ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct iof_projection_info	*fs_handle = fuse_req_userdata(req);
	struct iof_file_handle		*handle = NULL;
	struct TYPE_NAME		*desc = NULL;
	struct iof_gah_in		*in;
	int rc;

	if (fi)
		handle = (void *)fi->fh;

	IOF_TRACE_INFO(req, "ino %lu", ino);
	IOC_REQ_INIT_LL(desc, fs_handle, api, in, req, rc);
	if (rc)
		D_GOTO(err, rc);

	if (handle) {
		D_MUTEX_LOCK(&fs_handle->gah_lock);

		if (!H_GAH_IS_VALID(handle)) {
			D_MUTEX_UNLOCK(&fs_handle->gah_lock);
			D_GOTO(err, rc = EIO);
		}

		in->gah = handle->common.gah;
		D_MUTEX_UNLOCK(&fs_handle->gah_lock);
		IOF_TRACE_ALIAS(req, handle, "getattr_fh_fuse_req");
	} else {
		rc = find_gah(fs_handle, ino, &in->gah);
		if (rc != 0)
			D_GOTO(err, rc = ENOENT);
		if (in->gah.root != atomic_load_consume(&fs_handle->proj.grp->pri_srv_rank)) {
			IOF_TRACE_WARNING(fs_handle,
					  "Gah with old root %lu", ino);
			D_GOTO(err, rc = EHOSTDOWN);
		}
	}
	rc = iof_fs_send(&desc->request);
	if (rc != 0)
		D_GOTO(err, rc);
	return;
err:
	IOF_FUSE_REPLY_ERR(req, rc);
	if (desc)
		iof_pool_release(fs_handle->fgh_pool, desc);
}
