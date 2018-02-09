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
read_bulk_cb(const struct crt_cb_info *cb_info)
{
	struct iof_rb *rb = cb_info->cci_arg;
	struct iof_readx_out *out = crt_reply_get(cb_info->cci_rpc);
	int rc = 0;
	size_t bytes_read = 0;
	void *buff = NULL;

	if (cb_info->cci_rc != 0) {
		IOF_TRACE_INFO(rb->req, "Bad RPC reply %d", cb_info->cci_rc);
		rb->failure = true;
		D_GOTO(out, rc = EIO);
	}

	if (out->err) {
		IOF_TRACE_ERROR(rb->req, "Error from target %d", out->err);
		rb->failure = true;
		if (out->err == IOF_GAH_INVALID)
			rb->handle->common.gah_valid = 0;
		D_GOTO(out, rc = EIO);
	}

	if (out->rc)
		D_GOTO(out, rc = out->rc);

	if (out->iov_len > 0) {
		if (out->data.iov_len != out->iov_len)
			D_GOTO(out, rc = EIO);
		buff = out->data.iov_buf;
		bytes_read = out->data.iov_len;
	} else if (out->bulk_len > 0) {
		bytes_read = out->bulk_len;
		buff = rb->lb.buf;
	}

out:
	if (rc) {
		IOF_FUSE_REPLY_ERR(rb->req, rc);
	} else {
		STAT_ADD_COUNT(rb->fs_handle->stats, read_bytes, rc);

		/* It's not clear without benchmarking which approach is better
		 * here, fuse_reply_buf() is a small wrapper around writev()
		 * which is a much shorter code-path however fuse_reply_data()
		 * attempts to use splice which may well be faster.
		 *
		 * For now it's easy to pick between them, and both of them are
		 * passing valgrind tests.
		 */
		if (rb->fs_handle->flags & IOF_FUSE_READ_BUF) {
			rc = fuse_reply_buf(rb->req, buff, bytes_read);
			if (rc != 0)
				IOF_TRACE_ERROR(rb->req,
						"fuse_reply_buf returned %d:%s",
						rc, strerror(-rc));

		} else {
			rb->fbuf.buf[0].size = bytes_read;
			rb->fbuf.buf[0].mem = buff;
			rc = fuse_reply_data(rb->req, &rb->fbuf, 0);
			if (rc != 0)
				IOF_TRACE_ERROR(rb->req,
						"fuse_reply_data returned %d:%s",
						rc, strerror(-rc));
		}
		IOF_TRACE_DOWN(rb->req);
	}
	iof_pool_release(rb->pt, rb);
}

static void
ioc_read_bulk(struct iof_rb *rb, size_t len, off_t position,
	      struct iof_file_handle *handle)
{
	struct iof_readx_in *in = NULL;
	fuse_req_t req = rb->req;
	int rc;

	rc = crt_req_set_endpoint(rb->rpc, &handle->common.ep);
	if (rc)
		D_GOTO(out_err, rc = EIO);

	in = crt_req_get(rb->rpc);
	in->gah = handle->common.gah;
	in->xtvec.xt_off = position;
	in->xtvec.xt_len = len;
	in->data_bulk = rb->lb.handle;
	IOF_TRACE_LINK(rb->rpc, req, "read_bulk_rpc");

	crt_req_addref(rb->rpc);
	rc = crt_req_send(rb->rpc, read_bulk_cb, rb);
	if (rc) {
		crt_req_decref(rb->rpc);
		D_GOTO(out_err, rc = EIO);
	}

	iof_pool_restock(rb->pt);
	return;

out_err:
	IOF_FUSE_REPLY_ERR(req, rc);
	iof_pool_release(rb->pt, rb);
}

void ioc_ll_read(fuse_req_t req, fuse_ino_t ino, size_t len,
		 off_t position, struct fuse_file_info *fi)
{
	struct iof_file_handle *handle = (void *)fi->fh;
	struct iof_projection_info *fs_handle = handle->fs_handle;
	struct iof_pool_type *pt;
	struct iof_rb *rb = NULL;
	int rc;

	IOF_TRACE_INFO(req, "%#zx-%#zx " GAH_PRINT_STR, position,
		       position + len - 1, GAH_PRINT_VAL(handle->common.gah));

	if (FS_IS_OFFLINE(fs_handle))
		D_GOTO(out_err, rc = fs_handle->offline_reason);

	if (len <= 4096)
		pt = fs_handle->rb_pool_page;
	else
		pt = fs_handle->rb_pool_large;

	rb = iof_pool_acquire(pt);
	if (!rb)
		D_GOTO(out_err, rc = ENOMEM);
	IOF_TRACE_UP(rb, handle, "readbuf");
	IOF_TRACE_UP(req, rb, "read_fuse_req");

	rb->req = req;
	rb->handle = handle;
	rb->pt = pt;

	ioc_read_bulk(rb, len, position, handle);

	return;

out_err:
	IOF_FUSE_REPLY_ERR(req, rc);
}
