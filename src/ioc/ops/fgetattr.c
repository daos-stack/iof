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

int ioc_getattr_gah(struct iof_file_handle *handle, struct stat *stbuf)
{
	struct iof_projection_info *fs_handle = handle->fs_handle;
	struct getattr_req *req;
	struct iof_gah_in *in;
	int rc;

	STAT_ADD(fs_handle->stats, getfattr);
	IOF_TRACE_INFO(handle, GAH_PRINT_STR,
		       GAH_PRINT_VAL(handle->common.gah));
	if (!handle->common.gah_valid) {
		/* If the server has reported that the GAH is invalid
		 * then do not send a RPC to close it
		 */
		return -EIO;
	}
	IOC_RPC_INIT(fs_handle->fgh_pool, req, rpc, getattr_cb, rc);
	if (rc)
		return rc;

	in = crt_req_get(req->rpc);
	in->gah = handle->common.gah;
	req->reply.stat = stbuf;

	iof_fs_send(req, &(req)->reply.ctx);
	IOC_RPC_FINI(fs_handle->fgh_pool, req, rc);
	/* Cache the inode number */
	if (rc == 0)
		handle->inode_no = stbuf->st_ino;
	IOF_TRACE_DEBUG(handle, GAH_PRINT_STR " rc %d",
			GAH_PRINT_VAL(handle->common.gah), rc);
	return rc;
}

int ioc_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	if (fi)
		return ioc_getattr_gah((struct iof_file_handle *)fi->fh, stbuf);

	if (!path)
		return -EIO;
	return ioc_getattr_name(path, stbuf);
}

void
ioc_ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct iof_projection_info *fs_handle = fuse_req_userdata(req);
	struct iof_file_handle handle = {0};
	struct stat st = {0};
	int rc;

	IOF_LOG_INFO("Req %p %lu %p", req, ino, fi);

	rc = find_gah(fs_handle, ino, &handle.common.gah);
	if (rc != 0)
		fuse_reply_err(req, EIO);

	handle.fs_handle = fs_handle;
	handle.common.gah_valid = 1;
	rc = ioc_getattr_gah(&handle, &st);
	if (rc)
		fuse_reply_err(req, -rc);
	else
		fuse_reply_attr(req, &st, 0);
}
