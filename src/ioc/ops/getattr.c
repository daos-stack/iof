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
#define RESTOCK_ON_SEND
#include "ioc_ops.h"

void ioc_getattr_cb(struct ioc_request *request)
{
	struct iof_getattr_out *out = IOC_GET_RESULT(request);

	IOC_RESOLVE_STATUS(request, out);
	if (IOC_STATUS_TO_RC(request) == 0 && request->ptr != NULL)
		memcpy(request->ptr, out->stat.iov_buf, sizeof(struct stat));
}

static const struct ioc_request_api api = {
	.get_fsh	= get_fs_handle,
	.on_send	= post_send,
	.on_result	= ioc_getattr_cb,
	.on_evict	= ioc_simple_resend
};

#define STAT_KEY getattr

int ioc_getattr_name(const char *path, struct stat *stbuf)
{
	struct iof_projection_info *fs_handle = ioc_get_handle();
	struct TYPE_NAME *desc = NULL;
	struct iof_gah_string_in *in;

	int rc;

	if (strnlen(path, NAME_MAX) == NAME_MAX)
		return -EIO;

	IOF_LOG_INFO("path %s", path);
	IOC_REQ_INIT(desc, fs_handle, api, in, rc);
	if (rc)
		D_GOTO(out, rc);

	desc->request.ptr = stbuf;
	strncpy(in->name.name, path, NAME_MAX);
	IOC_REQ_SEND(desc, fs_handle, rc);
out:
	IOC_REQ_RELEASE(desc);
	IOF_TRACE_DEBUG(desc, "path %s rc %d", path, rc);
	return rc;
}

