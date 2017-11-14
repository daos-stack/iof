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

int ioc_unlink(const char *path)
{
	struct iof_projection_info *fs_handle = ioc_get_handle();
	struct iof_open_in *in;
	struct status_cb_r reply = {0};
	crt_rpc_t *rpc = NULL;
	int rc;

	STAT_ADD(fs_handle->stats, unlink);

	if (FS_IS_OFFLINE(fs_handle))
		return -fs_handle->offline_reason;

	if (strnlen(path, NAME_MAX) == NAME_MAX)
		return -EIO;

	if (!IOF_IS_WRITEABLE(fs_handle->flags)) {
		IOF_LOG_INFO("Attempt to modify Read-Only File System");
		return -EROFS;
	}

	IOF_LOG_INFO("path %s", path);

	rc = crt_req_create(fs_handle->proj.crt_ctx,
			    &fs_handle->proj.grp->psr_ep,
			    FS_TO_OP(fs_handle, unlink), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %d",
			      rc);
		return -EIO;
	}

	iof_tracker_init(&reply.tracker, 1);
	in = crt_req_get(rpc);
	strncpy(in->name.name, path, NAME_MAX);
	in->gah = fs_handle->gah;

	rc = crt_req_send(rpc, ioc_status_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send rpc, rc = %u", rc);
		return -EIO;
	}

	iof_fs_wait(&fs_handle->proj, &reply.tracker);

	IOF_LOG_DEBUG("path %s rc %d", path, IOC_STATUS_TO_RC(&reply));

	return IOC_STATUS_TO_RC(&reply);
}

static void
ioc_ll_remove(fuse_req_t req, fuse_ino_t parent, const char *name, bool dir)
{
	struct iof_projection_info *fs_handle = fuse_req_userdata(req);
	struct iof_open_in *in;
	crt_rpc_t *rpc = NULL;
	int rc;

	int ret = EIO;

	IOF_TRACE_UP(req, fs_handle, dir ? "rmdir" : "unlink");

	STAT_ADD(fs_handle->stats, unlink);

	if (FS_IS_OFFLINE(fs_handle)) {
		ret = fs_handle->offline_reason;
		goto out_err;
	}

	if (!IOF_IS_WRITEABLE(fs_handle->flags)) {
		IOF_LOG_INFO("Attempt to modify Read-Only File System");
		ret = EROFS;
		goto out_err;
	}

	rc = crt_req_create(fs_handle->proj.crt_ctx,
			    &fs_handle->proj.grp->psr_ep,
			    FS_TO_OP(fs_handle, unlink), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %d", rc);
		ret = EIO;
		goto out_err;
	}

	in = crt_req_get(rpc);
	strncpy(in->name.name, name, NAME_MAX);
	if (dir)
		in->flags = 1;

	/* Find the GAH of the parent */
	rc = find_gah(fs_handle, parent, &in->gah);
	if (rc != 0) {
		ret = ENOENT;
		goto out_err;
	}

	rc = crt_req_send(rpc, ioc_ll_gen_cb, req);
	if (rc) {
		IOF_TRACE_ERROR(req, "Could not send rpc, rc = %d", rc);
		ret = EIO;
		goto out_err;
	}

	return;
out_err:
	IOF_FUSE_REPLY_ERR(req, ret);

	if (rpc)
		crt_req_decref(rpc);
}

void
ioc_ll_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	ioc_ll_remove(req, parent, name, false);
}

void
ioc_ll_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	ioc_ll_remove(req, parent, name, true);
}
