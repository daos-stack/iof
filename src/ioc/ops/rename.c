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

void
ioc_ll_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
	      fuse_ino_t newparent, const char *newname, unsigned int flags)
{
	struct iof_projection_info *fs_handle = fuse_req_userdata(req);
	struct iof_rename_in	*in;
	crt_rpc_t		*rpc = NULL;
	int ret = EIO;
	int rc;

	IOF_TRACE_UP(req, fs_handle, "rename");

	STAT_ADD(fs_handle->stats, rename);

	IOF_TRACE_DEBUG(req, "renaming %s to %s", name, newname);

	if (FS_IS_OFFLINE(fs_handle))
		D_GOTO(err, ret = fs_handle->offline_reason);

	if (!IOF_IS_WRITEABLE(fs_handle->flags)) {
		IOF_LOG_INFO("Attempt to modify Read-Only File System");
		D_GOTO(err, ret = EROFS);
	}

	rc = crt_req_create(fs_handle->proj.crt_ctx,
			    &fs_handle->proj.grp->psr_ep,
			    FS_TO_OP(fs_handle, rename), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %d",
			      rc);
		D_GOTO(err, ret = ENOMEM);
	}

	in = crt_req_get(rpc);

	strncpy(in->old_name.name, name, NAME_MAX);
	strncpy(in->new_name.name, newname, NAME_MAX);
	in->flags = flags;

	/* Find the GAH of the parent */
	rc = find_gah(fs_handle, parent, &in->old_gah);
	if (rc != 0)
		D_GOTO(err, ret = ENOENT);

	rc = find_gah(fs_handle, newparent, &in->new_gah);
	if (rc != 0)
		D_GOTO(err, ret = ENOENT);

	rc = crt_req_send(rpc, ioc_ll_gen_cb, req);
	if (rc) {
		IOF_TRACE_ERROR(req, "Could not send rpc, rc = %d", rc);
		D_GOTO(err, ret = EIO);
	}

	return;

err:
	IOF_FUSE_REPLY_ERR(req, ret);

	if (rpc)
		crt_req_decref(rpc);
}
