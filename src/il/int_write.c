/* Copyright (C) 2017 Intel Corporation
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
#include "log.h"
#include "ios_gah.h"
#include "intercept.h"

/*
 * This will be defined by the calling function to select
 * the correct RPC type from the protocol registry.
 * This is used in the FS_TO_OP Macro below.
 */
#ifndef IOF_PROTO_CLASS
#define IOF_PROTO_CLASS DEFAULT
#endif

/*
 * Helpers for forcing macro expansion.
 */
#define EVAL_PROTO_CLASS(CLS) DEF_PROTO_CLASS(CLS)
#define EVAL_RPC_TYPE(CLS, TYPE) DEF_RPC_TYPE(CLS, TYPE)
/*
 * Returns the correct RPC Type ID from the protocol registry.
 */
#define FS_TO_OP(HANDLE, FN) \
		((&iof_protocol_registry[EVAL_PROTO_CLASS(IOF_PROTO_CLASS)])\
		  ->rpc_types[EVAL_RPC_TYPE(IOF_PROTO_CLASS, FN)].op_id)

struct write_cb_r {
	struct file_info *f_info;
	ssize_t len;
	struct iof_tracker tracker;
	int err;
	int rc;
};

#define BULK_THRESHOLD 64

static int write_cb(const struct crt_cb_info *cb_info)
{
	struct write_cb_r *reply = NULL;
	struct iof_write_out *out;

	reply = (struct write_cb_r *)cb_info->cci_arg;

	if (cb_info->cci_rc != 0) {
		/*
		 * Error handling.  On timeout return EAGAIN, all other errors
		 * return EIO.
		 *
		 * TODO: Handle target eviction here
		 */
		IOF_LOG_INFO("Bad RPC reply %d", cb_info->cci_rc);
		if (cb_info->cci_rc == -CER_TIMEDOUT)
			reply->err = EAGAIN;
		else
			reply->err = EIO;
		iof_tracker_signal(&reply->tracker);
		return 0;
	}

	out = crt_reply_get(cb_info->cci_rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not get reply");
		reply->err = EIO;
		iof_tracker_signal(&reply->tracker);
		return 0;
	}

	if (out->err) {
		IOF_LOG_ERROR("Error from target %d", out->err);

		reply->err = EIO;

		if (out->err == IOF_GAH_INVALID)
			reply->f_info->gah_valid = 0;

		if (out->err == IOF_ERR_NOMEM)
			reply->err = EAGAIN;

		iof_tracker_signal(&reply->tracker);
		return 0;
	}

	reply->len = out->len;
	reply->rc = out->rc;
	iof_tracker_signal(&reply->tracker);
	return 0;
}

static int write_direct(const char *buff, size_t len, off_t position,
			struct file_info *f_info)
{
	struct iof_projection *fs_handle;
	struct iof_service_group *grp;
	struct iof_write_in *in;
	struct write_cb_r reply = {0};
	crt_rpc_t *rpc = NULL;
	int rc;

	fs_handle = f_info->fs_handle;
	grp = fs_handle->grp;

	rc = crt_req_create(fs_handle->crt_ctx, grp->psr_ep,
			    FS_TO_OP(fs_handle, write_direct), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u",
			      rc);
		f_info->errcode = EIO;
		return -1;
	}

	iof_tracker_init(&reply.tracker, 1);
	in = crt_req_get(rpc);
	in->gah = f_info->gah;
	crt_iov_set(&in->data, (void *)buff, len);
	in->base = position;

	reply.f_info = f_info;

	rc = crt_req_send(rpc, write_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send open rpc, rc = %u", rc);
		f_info->errcode = EIO;
		return -1;
	}
	iof_fs_wait(fs_handle, &reply.tracker);

	if (reply.err) {
		f_info->errcode = reply.err;
		return -1;
	}

	if (reply.rc != 0) {
		f_info->errcode = reply.rc;
		return -1;
	}

	return reply.len;
}

static ssize_t write_bulk(const char *buff, size_t len, off_t position,
			  struct file_info *f_info)
{
	struct iof_projection *fs_handle;
	struct iof_service_group *grp;
	struct iof_write_bulk *in;
	struct write_cb_r reply = {0};
	crt_rpc_t *rpc = NULL;
	crt_bulk_t bulk;
	crt_sg_list_t sgl = {0};
	crt_iov_t iov = {0};
	int rc;

	fs_handle = f_info->fs_handle;
	grp = fs_handle->grp;

	rc = crt_req_create(fs_handle->crt_ctx, grp->psr_ep,
			    FS_TO_OP(fs_handle, write_bulk), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u",
			      rc);
		f_info->errcode = EIO;
		return -1;
	}

	in = crt_req_get(rpc);
	in->gah = f_info->gah;

	iov.iov_len = len;
	iov.iov_buf_len = len;
	iov.iov_buf = (void *)buff;
	sgl.sg_iovs = &iov;
	sgl.sg_nr.num = 1;

	rc = crt_bulk_create(fs_handle, &sgl, CRT_BULK_RO, &in->bulk);
	if (rc) {
		IOF_LOG_ERROR("Failed to make local bulk handle %d", rc);
		f_info->errcode = EIO;
		return -1;
	}

	iof_tracker_init(&reply.tracker, 1);
	in->base = position;

	bulk = in->bulk;

	reply.f_info = f_info;

	rc = crt_req_send(rpc, write_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send open rpc, rc = %u", rc);
		f_info->errcode = EIO;
		return -1;
	}
	iof_fs_wait(fs_handle, &reply.tracker);

	rc = crt_bulk_free(bulk);
	if (rc) {
		f_info->errcode = EIO;
		return -1;
	}

	if (reply.err) {
		f_info->errcode = reply.err;
		return -1;
	}

	if (reply.rc != 0) {
		f_info->errcode = reply.rc;
		return -1;
	}

	return reply.len;
}

ssize_t ioil_do_pwrite(const char *buff, size_t len, off_t position,
		       struct file_info *f_info)
{
	IOF_LOG_INFO("%#zx-%#zx " GAH_PRINT_STR, position, position + len - 1,
		     GAH_PRINT_VAL(f_info->gah));

	if (len >= BULK_THRESHOLD)
		return write_bulk(buff, len, position, f_info);
	else
		return write_direct(buff, len, position, f_info);
}

/* TODO: This could be optimized to send multiple RPCs at once rather than
 * sending them serially.   Get it working first.
 */
ssize_t ioil_do_pwritev(const struct iovec *iov, int count, off_t position,
			struct file_info *f_info)
{
	ssize_t bytes_written;
	ssize_t total_write = 0;
	int i;

	for (i = 0; i < count; i++) {
		if (iov[i].iov_len >= BULK_THRESHOLD)
			bytes_written = write_bulk(iov[i].iov_base,
						   iov[i].iov_len,
						   position, f_info);
		else
			bytes_written = write_direct(iov[i].iov_base,
						     iov[i].iov_len,
						     position, f_info);

		if (bytes_written == -1)
			return (ssize_t)-1;

		if (bytes_written == 0)
			return total_write;

		position += bytes_written;
		total_write += bytes_written;
	}

	return total_write;
}
