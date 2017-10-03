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

void getattr_cb(struct iof_rpc_ctx *ctx, void *output)
{
	struct iof_getattr_out *out = output;
	struct getattr_cb_r *reply = container_of(
				     ctx, struct getattr_cb_r, ctx);

	IOC_RESOLVE_STATUS(ctx, out);
	if (IOC_STATUS_TO_RC(ctx) == 0)
		memcpy(reply->stat, out->stat.iov_buf, sizeof(struct stat));
}

int ioc_getattr_name(const char *path, struct stat *stbuf)
{
	struct iof_projection_info *fs_handle = ioc_get_handle();
	struct getattr_req *req;
	struct iof_gah_string_in *in;

	int rc;

	STAT_ADD(fs_handle->stats, getattr);
	IOC_RPC_INIT(fs_handle->gh_pool, req, rpc, getattr_cb, rc);
	if (rc)
		return rc;

	IOF_TRACE_INFO(req, "path %s", path);
	in = crt_req_get(req->rpc);
	in->path = (d_string_t)path;
	req->reply.stat = stbuf;
	iof_fs_send(req, &(req)->reply.ctx);

	IOC_RPC_FINI(fs_handle->gh_pool, req, rc);
	IOF_TRACE_DEBUG(req, "path %s rc %d", path, rc);
	return rc;
}

