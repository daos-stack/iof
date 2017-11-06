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
#include "ios_gah.h"

#define REQ_NAME close_req
#define POOL_NAME dh_pool
#define TYPE_NAME iof_dir_handle
#define CONTAINER(req) container_of(req, struct TYPE_NAME, REQ_NAME)

static struct iof_projection_info
*get_fs_handle(struct ioc_request *req)
{
	return CONTAINER(req)->fs_handle;
}

static const struct ioc_request_api api = {
	.get_fsh	= get_fs_handle,
	/* No event handlers necessary */
};

int ioc_closedir(const char *dir, struct fuse_file_info *fi)
{
	struct iof_dir_handle *dh = (struct iof_dir_handle *)fi->fh;
	struct iof_projection_info *fs_handle = dh->fs_handle;
	struct iof_gah_in *in;
	int rc;

	STAT_ADD(fs_handle->stats, closedir);
	IOC_RPC_INIT(dh, REQ_NAME, fs_handle->POOL_NAME, api, rc);
	if (rc)
		return rc;

	IOF_TRACE_INFO(dh, GAH_PRINT_STR, GAH_PRINT_VAL(dh->gah));

	/* If the GAH has been reported as invalid by the server in the past
	 * then do not attempt to do anything with it.
	 *
	 * However, even if the local handle has been reported invalid then
	 * still continue to release the GAH on the server side.
	 */
	if (!dh->gah_valid) {
		rc = -EIO;
		goto out;
	}

	in = crt_req_get(dh->REQ_NAME.rpc);
	in->gah = dh->gah;
	rc = iof_fs_send(&dh->REQ_NAME);
	if (rc)
		goto out;
	IOC_RPC_WAIT(dh, REQ_NAME, fs_handle, rc);
out:
	iof_pool_release(fs_handle->POOL_NAME, dh);
	return rc;
}

void ioc_ll_releasedir(fuse_req_t req, fuse_ino_t ino,
		       struct fuse_file_info *fi)
{
	struct iof_dir_handle *handle = (struct iof_dir_handle *)fi->fh;
	struct iof_projection_info *fs_handle = handle->fs_handle;
	struct iof_gah_in *in;
	int ret = EIO;
	int rc;

	STAT_ADD(fs_handle->stats, release);

	/* If the projection is off-line then drop the local handle.
	 *
	 * This means a resource leak on the IONSS should the projection
	 * be offline for reasons other than IONSS failure.
	 */
	if (FS_IS_OFFLINE(fs_handle)) {
		ret = fs_handle->offline_reason;
		goto out_err;
	}

	IOF_TRACE_INFO(handle, GAH_PRINT_STR, GAH_PRINT_VAL(handle->gah));

	IOF_TRACE_LINK(req, handle, "request");

	if (!handle->gah_valid) {
		IOF_TRACE_INFO(handle, "Release with bad handle");

		/* If the server has reported that the GAH is invalid
		 * then do not send a RPC to close it.
		 */
		ret = EIO;
		goto out_err;
	}

	in = crt_req_get(handle->close_req.rpc);
	in->gah = handle->gah;

	crt_req_addref(handle->close_req.rpc);
	rc = crt_req_send(handle->close_req.rpc, ioc_ll_gen_cb, req);
	if (rc) {
		IOF_TRACE_ERROR(handle, "Could not send rpc, rc = %d", rc);
		ret = EIO;
		goto out_err;
	}

	iof_pool_release(fs_handle->dh_pool, handle);
	return;

out_err:
	IOF_FUSE_REPLY_ERR(req, ret);

	iof_pool_release(fs_handle->dh_pool, handle);
}
