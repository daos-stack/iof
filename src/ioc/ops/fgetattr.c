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
#include "ioc_ops.h"

#define STAT_KEY getattr

#define DECL_NAME(a, CB) ioc_##a##_##CB##_fn

static void ioc_getattr_result_fn(struct ioc_request *request)
{
	struct iof_attr_out *out = crt_reply_get(request->rpc);
	struct TYPE_NAME *desc;

	IOC_REQUEST_RESOLVE(request, out);

	if (request->rc == 0)
		IOC_REPLY_ATTR(request, &out->stat);
	else
		IOC_REPLY_ERR(request, request->rc);

	desc = CONTAINER(request);

	if (request->ir_ht == RHS_INODE)
		d_hash_rec_decref(&request->fsh->inode_ht,
				  &request->ir_inode->ie_htl);

	iof_pool_release(request->fsh->POOL_NAME, desc);
}

static void ioc_fsetattr_result_fn(struct ioc_request *request)
{
	struct iof_attr_out *out = crt_reply_get(request->rpc);

	IOC_REQUEST_RESOLVE(request, out);

	if (request->rc == 0)
		IOC_REPLY_ATTR(request, &out->stat);
	else
		IOC_REPLY_ERR(request, request->rc);

	iof_pool_release(request->fsh->POOL_NAME, CONTAINER(request));
}

static const struct ioc_request_api getattr_api = {
	.on_result	= ioc_getattr_result_fn,
	.on_evict	= ioc_simple_resend,
	.gah_offset	= offsetof(struct iof_gah_in, gah),
	.have_gah	= true,
};

static const struct ioc_request_api fgetattr_api = {
	.on_result	= ioc_fsetattr_result_fn,
	.on_evict	= ioc_simple_resend,
	.gah_offset	= offsetof(struct iof_gah_in, gah),
	.have_gah	= true,
};

void
ioc_ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct iof_projection_info	*fs_handle = fuse_req_userdata(req);
	struct iof_file_handle		*handle = NULL;
	struct TYPE_NAME		*desc = NULL;
	int rc;

	if (fi)
		handle = (void *)fi->fh;

	IOF_TRACE_INFO(fs_handle, "inode %lu handle %p", ino, handle);

	if (handle) {
		IOC_REQ_INIT_REQ(desc, fs_handle, fgetattr_api, req, rc);
		if (rc)
			D_GOTO(err, rc);

		desc->request.ir_ht = RHS_FILE;
		desc->request.ir_file = handle;

	} else {
		IOC_REQ_INIT_REQ(desc, fs_handle, getattr_api, req, rc);
		if (rc)
			D_GOTO(err, rc);

		if (ino == 1) {
			desc->request.ir_ht = RHS_ROOT;
		} else {
			rc = find_inode(fs_handle, ino, &desc->request.ir_inode);

			if (rc != 0) {
				IOF_TRACE_DOWN(&desc->request);
				D_GOTO(err, 0);
			}

			desc->request.ir_ht = RHS_INODE;
		}
	}
	rc = iof_fs_send(&desc->request);
	if (rc != 0)
		D_GOTO(err, rc);
	return;
err:
	IOC_REPLY_ERR_RAW(fs_handle, req, rc);
	if (desc)
		iof_pool_release(fs_handle->POOL_NAME, desc);
}
