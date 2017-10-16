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

#define REQ_NAME open_req
#define POOL_NAME dh_pool
#define TYPE_NAME iof_dir_handle
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

static void
opendir_cb(struct ioc_request *request)
{
	struct iof_opendir_out *out = IOC_GET_RESULT(request);
	struct iof_dir_handle *dh = CONTAINER(request);

	IOC_RESOLVE_STATUS(request, out);
	if (IOC_STATUS_TO_RC(request) == 0) {
		dh->gah = out->gah;
		dh->gah_valid = 1;
		dh->handle_valid = 1;
		dh->ep = dh->fs_handle->proj.grp->psr_ep;
	}
}

static const struct ioc_request_api api = {
	.get_fsh	= get_fs_handle,
	.on_send	= post_send,
	.on_result	= opendir_cb,
	.on_evict	= ioc_simple_resend
};

int ioc_opendir(const char *dir, struct fuse_file_info *fi)
{
	struct iof_projection_info *fs_handle = ioc_get_handle();
	struct iof_dir_handle *dh = NULL;
	struct iof_gah_string_in *in;
	int rc;

	STAT_ADD(fs_handle->stats, opendir);
	IOC_RPC_INIT(dh, REQ_NAME, fs_handle->POOL_NAME, api, rc);
	if (rc)
		goto out;
	IOF_TRACE_INFO(dh, "dir %s", dir);

	in = crt_req_get(dh->REQ_NAME.rpc);
	in->path = (d_string_t)dir;
	in->gah = fs_handle->gah;
	rc = iof_fs_send(&dh->REQ_NAME);
	if (rc)
		goto out;
	IOC_RPC_WAIT(dh, REQ_NAME, fs_handle, rc);

	IOF_TRACE_DEBUG(dh, "rc %d", rc);

out:
	if (rc == 0) {
		fi->fh = (uint64_t)dh;
		IOF_TRACE_DEBUG(dh, GAH_PRINT_FULL_STR,
				GAH_PRINT_FULL_VAL(dh->gah));
	} else {
		iof_pool_release(fs_handle->POOL_NAME, dh);
	}
	return rc;
}

static void
opendir_ll_cb(const struct crt_cb_info *cb_info)
{
	struct iof_opendir_out	*out = crt_reply_get(cb_info->cci_rpc);
	struct iof_dir_handle	*dir_handle = cb_info->cci_arg;
	struct fuse_file_info	fi = {0};
	fuse_req_t		req;
	int			ret = EIO;

	IOF_TRACE_DEBUG(dir_handle, "cci_rc %d rc %d err %d",
			cb_info->cci_rc, out->rc, out->err);

	if (cb_info->cci_rc != 0)
		goto out_err;

	if (out->rc) {
		ret = out->rc;
		goto out_err;
	}

	if (out->err)
		goto out_err;

	/* Create a new FI desciptor and use it to point to
	 * our local handle
	 */

	fi.fh = (uint64_t)dir_handle;
	dir_handle->gah = out->gah;
	dir_handle->gah_valid = 1;
	dir_handle->handle_valid = 1;
	req = dir_handle->open_f_req;
	dir_handle->open_f_req = 0;

	fuse_reply_open(req, &fi);
	return;

out_err:
	IOF_FUSE_REPLY_ERR(dir_handle->open_f_req, ret);
	iof_pool_release(dir_handle->fs_handle->dh_pool, dir_handle);
}

void ioc_ll_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct iof_projection_info *fs_handle = fuse_req_userdata(req);
	struct iof_dir_handle *dir_handle = NULL;
	struct iof_gah_string_in *in;
	int ret = EIO;
	int rc;

	IOF_TRACE_INFO(req, "ino %lu", ino);

	STAT_ADD(fs_handle->stats, opendir);

	if (FS_IS_OFFLINE(fs_handle)) {
		ret = fs_handle->offline_reason;
		goto out_err;
	}

	dir_handle = iof_pool_acquire(fs_handle->dh_pool);
	if (!dir_handle) {
		ret = ENOMEM;
		goto out_err;
	}

	IOF_TRACE_LINK(req, dir_handle, "request");

	dir_handle->ep = fs_handle->proj.grp->psr_ep;
	dir_handle->open_f_req = req;

	in = crt_req_get(dir_handle->open_req.rpc);

	/* Find the GAH of the parent */
	rc = find_gah(fs_handle, ino, &in->gah);
	if (rc != 0) {
		ret = ENOENT;
		goto out_err;
	}

	dir_handle->ep = fs_handle->proj.grp->psr_ep;

	rc = crt_req_set_endpoint(dir_handle->open_req.rpc, &dir_handle->ep);
	if (rc) {
		IOF_TRACE_ERROR(dir_handle, "Could not set ep, rc = %d", rc);
		ret = EIO;
		goto out_err;
	}

	rc = crt_req_set_endpoint(dir_handle->close_req.rpc, &dir_handle->ep);
	if (rc) {
		IOF_TRACE_ERROR(dir_handle, "Could not set ep, rc = %d", rc);
		ret = EIO;
		goto out_err;
	}

	rc = crt_req_send(dir_handle->open_req.rpc, opendir_ll_cb, dir_handle);
	if (rc) {
		IOF_TRACE_ERROR(dir_handle, "Could not send rpc, rc = %d", rc);
		ret = EIO;
		goto out_err;
	}
	crt_req_addref(dir_handle->open_req.rpc);

	iof_pool_restock(fs_handle->dh_pool);

	return;

out_err:
	IOF_FUSE_REPLY_ERR(req, ret);

	if (dir_handle)
		iof_pool_release(fs_handle->dh_pool, dir_handle);
}
