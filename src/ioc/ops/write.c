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

		if (out->err == -DER_NONEXIST)
			H_GAH_SET_INVALID(wb->handle);

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
		wb->failure = true;
err:
	IOF_FUSE_REPLY_ERR(req, rc);

	iof_pool_release(wb->fs_handle->write_pool, wb);
}

static void
ioc_writex(size_t len, off_t position, struct iof_wb *wb,
	   struct iof_file_handle *handle)
{
	struct iof_writex_in *in = crt_req_get(wb->rpc);
	int rc;

	IOF_TRACE_LINK(wb->rpc, wb->req, "writex_rpc");

	rc = crt_req_set_endpoint(wb->rpc, &handle->common.ep);
	if (rc) {
		IOF_TRACE_ERROR(wb->req, "Could not set endpoint, rc = %d",
				rc);
		D_GOTO(err, rc = EIO);
	}

	D_MUTEX_LOCK(&handle->fs_handle->gah_lock);
	in->gah = handle->common.gah;
	D_MUTEX_UNLOCK(&handle->fs_handle->gah_lock);

	in->xtvec.xt_len = len;
	if (len <= handle->fs_handle->proj.max_iov_write) {
		d_iov_set(&in->data, wb->lb.buf, len);
	} else {
		in->bulk_len = len;
		in->data_bulk = wb->lb.handle;
	}

	in->xtvec.xt_off = position;

	crt_req_addref(wb->rpc);
	rc = crt_req_send(wb->rpc, write_cb, wb);
	if (rc) {
		crt_req_decref(wb->rpc);
		D_GOTO(err, rc = EIO);
	}

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

	STAT_ADD(handle->fs_handle->stats, write);

	if (FS_IS_OFFLINE(handle->fs_handle))
		D_GOTO(err, rc = handle->fs_handle->offline_reason);

	/* If the server has reported that the GAH is invalid then do not try
	 * and use it.
	 */
	if (!F_GAH_IS_VALID(handle))
		D_GOTO(err, rc = EIO);

	wb = iof_pool_acquire(handle->fs_handle->write_pool);
	if (!wb)
		D_GOTO(err, rc = ENOMEM);

	IOF_TRACE_UP(wb, handle, "writebuf");
	IOF_TRACE_UP(req, wb, "write_fuse_req");

	IOF_TRACE_INFO(req, "%#zx-%#zx " GAH_PRINT_STR, position,
		       position + len - 1, GAH_PRINT_VAL(handle->common.gah));

	wb->req = req;
	wb->handle = handle;

	memcpy(wb->lb.buf, buff, len);

	ioc_writex(len, position, wb, handle);

	return;
err:
	IOF_FUSE_REPLY_ERR(req, rc);
	if (wb)
		iof_pool_release(handle->fs_handle->write_pool, wb);
}

/*
 * write_buf() callback for fuse.  Essentially the same as ioc_ll_write()
 * however with two advantages, it allows us to check parameters before
 * doing any allocation/memcpy() and it uses fuse_buf_copy() to put the data
 * directly into our data buffer avoiding an additional memcpy().
 */
void ioc_ll_write_buf(fuse_req_t req, fuse_ino_t ino, struct fuse_bufvec *bufv,
		      off_t position, struct fuse_file_info *fi)
{
	struct iof_file_handle *handle = (struct iof_file_handle *)fi->fh;
	struct iof_wb *wb = NULL;
	size_t len = bufv->buf[0].size;
	struct fuse_bufvec dst = { .count = 1 };
	int rc;

	STAT_ADD(handle->fs_handle->stats, write);

	if (FS_IS_OFFLINE(handle->fs_handle))
		D_GOTO(err, rc = handle->fs_handle->offline_reason);

	/* If the server has reported that the GAH is invalid then do not try
	 * and use it.
	 */
	if (!F_GAH_IS_VALID(handle))
		D_GOTO(err, rc = EIO);

	/* Check for buffer count being 1.  According to the documentation this
	 * will always be the case, and if it isn't then our code will be using
	 * the wrong value for len
	 */
	if (bufv->count != 1)
		D_GOTO(err, rc = EIO);

	IOF_TRACE_INFO(handle, "Count %zi [0].flags %#x",
		       bufv->count, bufv->buf[0].flags);

	wb = iof_pool_acquire(handle->fs_handle->write_pool);
	if (!wb)
		D_GOTO(err, rc = ENOMEM);
	IOF_TRACE_UP(wb, handle, "writebuf");
	IOF_TRACE_UP(req, wb, "write_buf_fuse_req");

	IOF_TRACE_INFO(req, "%#zx-%#zx " GAH_PRINT_STR, position,
		       position + len - 1, GAH_PRINT_VAL(handle->common.gah));

	wb->req = req;
	wb->handle = handle;

	dst.buf[0].size = len;
	dst.buf[0].mem = wb->lb.buf;
	rc = fuse_buf_copy(&dst, bufv, 0);
	if (rc != len)
		D_GOTO(err, rc = EIO);

	ioc_writex(len, position, wb, handle);

	return;
err:
	IOF_FUSE_REPLY_ERR(req, rc);
	if (wb)
		iof_pool_release(handle->fs_handle->write_pool, wb);
}
