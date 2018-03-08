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
#include "ios_gah.h"

static void
ioc_open_ll_cb(const struct crt_cb_info *cb_info)
{
	struct iof_file_handle	*handle = cb_info->cci_arg;
	struct iof_open_out	*out = crt_reply_get(cb_info->cci_rpc);
	struct fuse_file_info	fi = {0};
	fuse_req_t		req;
	int			ret = EIO;

	IOF_TRACE_DEBUG(handle, "cci_rc %d rc %d err %d",
			cb_info->cci_rc, out->rc, out->err);

	if (cb_info->cci_rc != 0)
		goto out_err;

	if (out->rc) {
		ret = out->rc;
		goto out_err;
	}

	if (out->err)
		goto out_err;

	/* Create a new FI descriptor and use it to point to
	 * our local handle
	 */

	fi.fh = (uint64_t)handle;
	handle->common.gah = out->gah;
	H_GAH_SET_VALID(handle);
	pthread_mutex_lock(&handle->fs_handle->of_lock);
	d_list_add_tail(&handle->list, &handle->fs_handle->openfile_list);
	pthread_mutex_unlock(&handle->fs_handle->of_lock);
	req = handle->open_req;
	handle->open_req = 0;

	IOF_FUSE_REPLY_OPEN(req, fi);

	return;

out_err:
	IOF_FUSE_REPLY_ERR(handle->open_req, ret);
	iof_pool_release(handle->fs_handle->fh_pool, handle);
}

void ioc_ll_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct iof_projection_info *fs_handle = fuse_req_userdata(req);
	struct iof_file_handle *handle = NULL;
	struct iof_open_in *in;
	int ret = EIO;
	int rc;

	STAT_ADD(fs_handle->stats, open);

	if (FS_IS_OFFLINE(fs_handle)) {
		ret = fs_handle->offline_reason;
		goto out_err;
	}

	/* O_LARGEFILE should always be set on 64 bit systems, and in fact is
	 * defined to 0 so IOF defines LARGEFILE to the value that O_LARGEFILE
	 * would otherwise be using and check that is set.
	 */
	if (!(fi->flags & LARGEFILE)) {
		IOF_TRACE_INFO(fs_handle, "O_LARGEFILE required 0%o",
			       fi->flags);
		ret = ENOTSUP;
		goto out_err;
	}

	/* Check for flags that do not make sense in this context.
	 */
	if (fi->flags & IOF_UNSUPPORTED_OPEN_FLAGS) {
		IOF_TRACE_INFO(fs_handle, "unsupported flag requested 0%o",
			       fi->flags);
		ret = ENOTSUP;
		goto out_err;
	}

	if (fi->flags & O_WRONLY || fi->flags & O_RDWR) {
		if (!IOF_IS_WRITEABLE(fs_handle->flags)) {
			IOF_TRACE_INFO("Attempt to modify "
				       "Read-Only File System");
			ret = EROFS;
			goto out_err;
		}
	}

	handle = iof_pool_acquire(fs_handle->fh_pool);
	if (!handle) {
		ret = ENOMEM;
		goto out_err;
	}
	IOF_TRACE_UP(handle, fs_handle, fs_handle->fh_pool->reg.name);
	IOF_TRACE_UP(req, handle, "open_fuse_req");

	handle->common.projection = &fs_handle->proj;
	handle->open_req = req;

	in = crt_req_get(handle->open_rpc);
	IOF_TRACE_LINK(handle->open_rpc, req, "open_file_rpc");

	/* Find the GAH of the file to open */
	rc = find_gah(fs_handle, ino, &in->gah);
	if (rc != 0) {
		ret = ENOENT;
		goto out_err;
	}

	in->flags = fi->flags;
	IOF_TRACE_INFO(req, "flags 0%o", fi->flags);

	crt_req_addref(handle->open_rpc);
	rc = crt_req_send(handle->open_rpc, ioc_open_ll_cb, handle);
	if (rc) {
		crt_req_decref(handle->open_rpc);
		D_GOTO(out_err, ret = EIO);
	}

	iof_pool_restock(fs_handle->fh_pool);

	LOG_FLAGS(handle, fi->flags);

	return;
out_err:
	IOF_FUSE_REPLY_ERR(req, ret);

	if (handle)
		iof_pool_release(fs_handle->fh_pool, handle);
}
