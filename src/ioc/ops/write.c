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

static void
write_cb(const struct crt_cb_info *cb_info)
{
	struct iof_wb		*wb = cb_info->cci_arg;
	struct iof_writex_out	*out = crt_reply_get(cb_info->cci_rpc);
	struct iof_writex_in	*in = crt_req_get(wb->rpc);
	fuse_req_t		req = wb->req;
	int rc;

	if (cb_info->cci_rc != 0) {
		IOF_TRACE_INFO(req, "Bad RPC reply %d", cb_info->cci_rc);
		D_GOTO(hard_err, rc = EIO);
	}

	if (out->err) {
		/* Convert the error types, out->err is a IOF error code
		 * so translate it to a errno we can pass back to FUSE.
		 */
		IOF_TRACE_ERROR(req, "Error from target %d", out->err);

		if (out->err == IOF_GAH_INVALID)
			wb->handle->common.gah_valid = 0;

		D_GOTO(hard_err, rc = EIO);
		return;
	}

	if (out->rc)
		D_GOTO(err, rc = out->rc);

	IOF_FUSE_REPLY_WRITE(req, out->len);

	STAT_ADD_COUNT(wb->fs_handle->stats, write_bytes, out->len);

	iof_pool_release(wb->fs_handle->write_pool, wb);

	return;

hard_err:
	/* This is overly cautious however if there is any non-I/O error
	 * returned after submitting the RPC then recreate the bulk handle
	 * before reuse.
	 */
	if (in->data_bulk)
		wb->error = true;
err:
	IOF_FUSE_REPLY_ERR(req, rc);

	iof_pool_release(wb->fs_handle->write_pool, wb);
}

static void
ioc_writex(const char *buff, size_t len, off_t position,
	   struct iof_wb *wb, struct iof_file_handle *handle)
{
	struct iof_writex_in *in = crt_req_get(wb->rpc);
	int rc;

	rc = crt_req_set_endpoint(wb->rpc, &handle->common.ep);
	if (rc) {
		IOF_TRACE_ERROR(handle, "Could not set endpoint, rc = %u",
				rc);
		D_GOTO(err, rc = EIO);
	}
	IOF_TRACE_LINK(wb, handle, "writex_rpc");

	in->gah = handle->common.gah;

	memcpy(wb->lb.buf, buff, len);

	in->xtvec.xt_len = len;
	if (len <= handle->fs_handle->proj.max_iov_write) {
		d_iov_set(&in->data, wb->lb.buf, len);
	} else {
		in->bulk_len = len;
		in->data_bulk = wb->lb.handle;
	}

	in->xtvec.xt_off = position;

	rc = crt_req_send(wb->rpc, write_cb, wb);
	if (rc) {
		IOF_TRACE_ERROR(handle, "Could not send rpc, rc = %u", rc);
		D_GOTO(err, rc = EIO);
	}
	crt_req_addref(wb->rpc);

	return;

err:
	IOF_FUSE_REPLY_ERR(wb->req, rc);
	iof_pool_release(handle->fs_handle->write_pool, wb);
}

void ioc_ll_write(fuse_req_t req, fuse_ino_t ino, const char *buff, size_t len,
		  off_t position, struct fuse_file_info *fi)
{
	struct iof_file_handle *handle = (struct iof_file_handle *)fi->fh;
	struct iof_wb *wb = NULL;
	int rc;

	IOF_TRACE_UP(req, handle, "write");

	STAT_ADD(handle->fs_handle->stats, write);

	if (FS_IS_OFFLINE(handle->fs_handle))
		D_GOTO(err, rc = handle->fs_handle->offline_reason);

	if (!handle->common.gah_valid)
		/* If the server has reported that the GAH is invalid
		 * then do not try and use it.
		 */
		D_GOTO(err, rc = EIO);

	wb = iof_pool_acquire(handle->fs_handle->write_pool);
	if (!wb)
		D_GOTO(err, rc = ENOMEM);

	IOF_TRACE_INFO(wb, "%#zx-%#zx " GAH_PRINT_STR, position,
		       position + len - 1, GAH_PRINT_VAL(handle->common.gah));

	wb->req = req;
	wb->handle = handle;

	ioc_writex(buff, len, position, wb, handle);

	return;
err:
	IOF_FUSE_REPLY_ERR(req, rc);
	if (wb)
		iof_pool_release(handle->fs_handle->write_pool, wb);
}
