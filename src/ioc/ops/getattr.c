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

#define REQ_NAME request
#define POOL_NAME gh_pool
#define TYPE_NAME getattr_req
#define CONTAINER(req) container_of(req, struct TYPE_NAME, REQ_NAME)

static struct iof_projection_info
*get_fs_handle(struct ioc_request *req)
{
	return CONTAINER(req)->fs_handle;
}

static void post_send(struct ioc_request *req)
{
	iof_pool_restock(CONTAINER(req)->fs_handle->POOL_NAME);
}

void ioc_getattr_cb(struct ioc_request *request)
{
	struct iof_getattr_out *out = IOC_GET_RESULT(request);

	IOC_RESOLVE_STATUS(request, out);
	if (IOC_STATUS_TO_RC(request) == 0)
		memcpy(request->ptr, out->stat.iov_buf, sizeof(struct stat));
}

static const struct ioc_request_api api = {
	.get_fsh	= get_fs_handle,
	.on_send	= post_send,
	.on_result	= ioc_getattr_cb,
	.on_evict	= ioc_simple_resend
};

int ioc_getattr_name(const char *path, struct stat *stbuf)
{
	struct iof_projection_info *fs_handle = ioc_get_handle();
	struct getattr_req *req = NULL;
	struct iof_gah_string_in *in;

	int rc;

	STAT_ADD(fs_handle->stats, getattr);
	IOF_LOG_INFO("path %s", path);
	IOC_RPC_INIT(req, request, fs_handle->gh_pool, api, rc);
	if (rc)
		return rc;

	req->request.ptr = stbuf;
	in = crt_req_get(req->request.rpc);
	in->path = (d_string_t)path;
	iof_fs_send(&req->request);
	IOC_RPC_WAIT(req, request, fs_handle, rc);
	iof_pool_release(fs_handle->gh_pool, req);
	IOF_TRACE_DEBUG(req, "path %s rc %d", path, rc);
	return rc;
}

