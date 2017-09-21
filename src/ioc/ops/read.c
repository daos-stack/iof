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

struct read_cb_r {
	void *out;
	struct iof_file_handle *handle;
	crt_rpc_t *rpc;
	struct iof_tracker tracker;
	int err;
	int rc;
};

static void
read_cb(const struct crt_cb_info *cb_info)
{
	struct read_cb_r *reply = cb_info->cci_arg;
	struct iof_data_out *out = crt_reply_get(cb_info->cci_rpc);
	int rc;

	if (cb_info->cci_rc != 0) {
		/*
		 * Error handling.  On timeout return EAGAIN, all other errors
		 * return EIO.
		 *
		 * TODO: Handle target eviction here
		 */
		IOF_LOG_INFO("Bad RPC reply %d", cb_info->cci_rc);
		if (cb_info->cci_rc == -DER_TIMEDOUT)
			reply->err = EAGAIN;
		else
			reply->err = EIO;
		iof_tracker_signal(&reply->tracker);
		return;
	}

	if (out->err) {
		IOF_LOG_ERROR("Error from target %d", out->err);

		if (out->err == IOF_GAH_INVALID)
			reply->handle->common.gah_valid = 0;

		reply->err = EIO;
		iof_tracker_signal(&reply->tracker);
		return;
	}

	if (out->rc) {
		reply->rc = out->rc;
		iof_tracker_signal(&reply->tracker);
		return;
	}

	rc = crt_req_addref(cb_info->cci_rpc);
	if (rc) {
		IOF_LOG_ERROR("could not take reference on query RPC, rc = %d",
			      rc);
		reply->err = EIO;
	} else {
		reply->out = out;
		reply->rpc = cb_info->cci_rpc;
	}

	iof_tracker_signal(&reply->tracker);
}

static void
read_bulk_cb(const struct crt_cb_info *cb_info)
{
	struct read_cb_r *reply = cb_info->cci_arg;
	struct iof_read_bulk_out *out = crt_reply_get(cb_info->cci_rpc);
	int rc;

	if (cb_info->cci_rc != 0) {
		/*
		 * Error handling.  On timeout return EAGAIN, all other errors
		 * return EIO.
		 *
		 * TODO: Handle target eviction here
		 */
		IOF_LOG_INFO("Bad RPC reply %d", cb_info->cci_rc);
		if (cb_info->cci_rc == -DER_TIMEDOUT)
			reply->err = EAGAIN;
		else
			reply->err = EIO;
		iof_tracker_signal(&reply->tracker);
		return;
	}

	if (out->err) {
		IOF_LOG_ERROR("Error from target %d", out->err);

		if (out->err == IOF_GAH_INVALID)
			reply->handle->common.gah_valid = 0;

		if (out->err == IOF_ERR_NOMEM)
			reply->err = ENOMEM;
		else
			reply->err = EIO;
		iof_tracker_signal(&reply->tracker);
		return;
	}

	if (out->rc) {
		reply->rc = out->rc;
		iof_tracker_signal(&reply->tracker);
		return;
	}

	rc = crt_req_addref(cb_info->cci_rpc);
	if (rc) {
		IOF_LOG_ERROR("could not take reference on query RPC, rc = %d",
			      rc);
		reply->err = EIO;
	} else {
		reply->out = out;
		reply->rpc = cb_info->cci_rpc;
	}

	iof_tracker_signal(&reply->tracker);
}

static int
ioc_read_direct(struct iof_rb *rb, size_t len, off_t position,
		struct iof_file_handle *handle)
{
	struct iof_projection_info *fs_handle = handle->fs_handle;
	struct iof_read_in *in;
	struct iof_data_out *out;
	struct read_cb_r reply = {0};
	char *buff = rb->buf.buf[0].mem;
	crt_rpc_t *rpc = NULL;
	int rc;

	rc = crt_req_create(fs_handle->proj.crt_ctx, &handle->common.ep,
			    FS_TO_OP(fs_handle, read), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u",
			      rc);
		return -EIO;
	}

	iof_tracker_init(&reply.tracker, 1);
	in = crt_req_get(rpc);
	in->gah = handle->common.gah;
	in->base = position;
	in->len = len;

	reply.handle = handle;

	rc = crt_req_send(rpc, read_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send open rpc, rc = %u", rc);
		return -EIO;
	}
	iof_pool_restock(fs_handle->rb_pool_small);

	iof_fs_wait(&fs_handle->proj, &reply.tracker);

	if (reply.err)
		return -reply.err;

	if (reply.rc != 0)
		return -reply.rc;

	out = reply.out;
	len = out->data.iov_len;

	if (len > 0)
		memcpy(buff, out->data.iov_buf, out->data.iov_len);

	rc = crt_req_decref(reply.rpc);
	if (rc)
		IOF_LOG_ERROR("decref returned %d", rc);

	return len;
}

static int
ioc_read_bulk(struct iof_rb *rb, size_t len, off_t position,
	      struct iof_file_handle *handle)
{
	struct iof_projection_info *fs_handle = handle->fs_handle;
	struct iof_read_bulk_in *in;
	struct iof_read_bulk_out *out;
	struct read_cb_r reply = {0};
	char *buff = rb->buf.buf[0].mem;
	crt_rpc_t *rpc = NULL;
	crt_bulk_t bulk;
	d_sg_list_t sgl = {0};
	d_iov_t iov = {0};
	int rc;

	rc = crt_req_create(fs_handle->proj.crt_ctx, &handle->common.ep,
			    FS_TO_OP(fs_handle, read_bulk), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u",
			      rc);
		return -EIO;
	}

	in = crt_req_get(rpc);
	in->gah = handle->common.gah;
	in->base = position;
	in->len = len;

	iov.iov_len = len;
	iov.iov_buf_len = len;
	iov.iov_buf = (void *)buff;
	sgl.sg_iovs = &iov;
	sgl.sg_nr.num = 1;

	rc = crt_bulk_create(fs_handle->proj.crt_ctx, &sgl, CRT_BULK_RW,
			     &in->bulk);
	if (rc) {
		IOF_LOG_ERROR("Failed to make local bulk handle %d", rc);
		return -EIO;
	}

	bulk = in->bulk;

	iof_tracker_init(&reply.tracker, 1);
	reply.handle = handle;

	rc = crt_req_send(rpc, read_bulk_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send open rpc, rc = %u", rc);
		return -EIO;
	}
	if (len <= 4096)
		iof_pool_restock(fs_handle->rb_pool_page);
	else
		iof_pool_restock(fs_handle->rb_pool_large);
	iof_fs_wait(&fs_handle->proj, &reply.tracker);

	if (reply.err) {
		crt_bulk_free(bulk);
		return -reply.err;
	}

	if (reply.rc != 0) {
		crt_bulk_free(bulk);
		return -reply.rc;
	}

	out = reply.out;
	if (out->iov_len > 0) {
		if (out->data.iov_len != out->iov_len) {
			/* TODO: This is a resource leak */
			IOF_LOG_ERROR("Missing IOV %d", out->iov_len);
			return -EIO;
		}
		len = out->data.iov_len;
		memcpy(buff, out->data.iov_buf, len);
	} else {
		len = out->bulk_len;
		IOF_LOG_INFO("Received %#zx via bulk", len);
	}

	rc = crt_req_decref(reply.rpc);
	if (rc)
		IOF_LOG_ERROR("decref returned %d", rc);

	rc = crt_bulk_free(bulk);
	if (rc)
		return -EIO;

	IOF_LOG_INFO("Read complete %#zx", len);

	return len;
}

int
ioc_read_buf(const char *file, struct fuse_bufvec **bufp, size_t len,
	     off_t position, struct fuse_file_info *fi)
{
	struct iof_file_handle *handle = (struct iof_file_handle *)fi->fh;
	struct iof_projection_info *fs_handle = handle->fs_handle;
	struct fuse_bufvec *buf;
	struct iof_rb *rb;
	struct iof_pool_type *pt;
	int rc;

	STAT_ADD(fs_handle->stats, read);

	if (FS_IS_OFFLINE(fs_handle))
		return -fs_handle->offline_reason;

	IOF_LOG_INFO("%#zx-%#zx " GAH_PRINT_STR, position, position + len - 1,
		     GAH_PRINT_VAL(handle->common.gah));

	if (!handle->common.gah_valid) {
		/* If the server has reported that the GAH is invalid
		 * then do not send a RPC to close it
		 */
		return -EIO;
	}

	if (len <= fs_handle->max_iov_read)
		pt = fs_handle->rb_pool_small;
	else if (len <= 4096)
		pt = fs_handle->rb_pool_page;
	else
		pt = fs_handle->rb_pool_large;

	rb = iof_pool_acquire(pt);
	if (!rb)
		return -ENOMEM;

	buf = &rb->buf;

	IOF_LOG_DEBUG("Using buffer at %p", buf->buf[0].mem);

	if (len > fs_handle->max_iov_read)
		rc = ioc_read_bulk(rb, len, position, handle);
	else
		rc = ioc_read_direct(rb, len, position, handle);


	/* In theory for zero-sized reads FUSE could do without a buffer here
	 * as there are no file contents to describe however the API requires
	 * one.
	 */

	if (rc >= 0) {
		STAT_ADD_COUNT(fs_handle->stats, read_bytes, rc);
		buf->buf[0].size = rc;
		*bufp = buf;
		iof_pool_consume(pt, rb);
	} else {
		iof_pool_release(pt, rb);
	}

	IOF_LOG_INFO("Read complete %i", rc);

	return rc;
}
