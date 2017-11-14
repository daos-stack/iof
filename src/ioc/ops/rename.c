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

static int
ioc_rename_priv(const char *oldpath, const char *newpath)
{
	struct iof_projection_info *fs_handle = ioc_get_handle();
	struct iof_two_string_in *in;
	struct status_cb_r reply = {0};
	crt_rpc_t *rpc = NULL;
	int rc;

	STAT_ADD(fs_handle->stats, rename);

	if (FS_IS_OFFLINE(fs_handle))
		return -fs_handle->offline_reason;

	if (strnlen(newpath, NAME_MAX) == NAME_MAX)
		return -EIO;

	if (!IOF_IS_WRITEABLE(fs_handle->flags)) {
		IOF_LOG_INFO("Attempt to modify Read-Only File System");
		return -EROFS;
	}

	IOF_LOG_INFO("oldpath %s newpath %s", oldpath, newpath);

	rc = crt_req_create(fs_handle->proj.crt_ctx,
			    &fs_handle->proj.grp->psr_ep,
			    FS_TO_OP(fs_handle, rename), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u",
			      rc);
		return -EIO;
	}

	iof_tracker_init(&reply.tracker, 1);
	in = crt_req_get(rpc);
	in->oldpath = (d_string_t)oldpath;
	strncpy(in->common.name.name, newpath, NAME_MAX);
	in->common.gah = fs_handle->gah;

	rc = crt_req_send(rpc, ioc_status_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send rpc, rc = %u", rc);
		return -EIO;
	}
	iof_fs_wait(&fs_handle->proj, &reply.tracker);

	IOF_LOG_DEBUG("path %s rc %d", oldpath, IOC_STATUS_TO_RC(&reply));

	return IOC_STATUS_TO_RC(&reply);
}

int ioc_rename(const char *oldpath, const char *newpath, unsigned int flags)
{
	if (flags) {
		IOF_LOG_INFO("Unsupported rename flags %x", flags);
		return -ENOTSUP;
	}
	return ioc_rename_priv(oldpath, newpath);
}

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
			    FS_TO_OP(fs_handle, rename_ll), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u",
			      rc);
		D_GOTO(err, ret = ENOMEM);
	}

	in = crt_req_get(rpc);

	in->old_path = (d_string_t)name;
	in->new_path = (d_string_t)newname;
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
