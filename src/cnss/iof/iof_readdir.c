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

#ifdef IOF_USE_FUSE3
#include <fuse3/fuse.h>
#else
#include <fuse/fuse.h>
#endif

#include "iof_common.h"
#include "iof.h"
#include "log.h"
#include "ios_gah.h"

struct readdir_cb_r {
	int complete;
	crt_rpc_t *rpc;
	int err;
	struct iof_readdir_out *out;
};

/* The callback of the readdir RPC.
 *
 * All this function does is take a reference on the data and return.
 */
static int readdir_cb(const struct crt_cb_info *cb_info)
{
	struct readdir_cb_r *reply = cb_info->cci_arg;
	int ret;

	if (cb_info->cci_rc != 0) {
		/* Error handling, as directory handles are stateful if there
		 * is any error then we have to disable the local dir_handle
		 *
		 */
		reply->err = EIO;
		reply->complete = 1;
		return 0;
	}

	reply->out = crt_reply_get(cb_info->cci_rpc);
	if (!reply->out) {
		reply->err = EIO;
		IOF_LOG_ERROR("Could not get query reply");
		reply->complete = 1;
		return 0;
	}

	ret = crt_req_addref(cb_info->cci_rpc);
	if (ret) {
		reply->err = EIO;
		IOF_LOG_ERROR("could not take reference on query RPC, ret = %d",
			      ret);
		reply->complete = 1;
		return 0;
	}

	reply->rpc = cb_info->cci_rpc;
	reply->complete = 1;
	return 0;
}

/*
 * Send, and wait for a readdir() RPC.  Populate the dir_handle with the
 * replies, count and rpc which a reference is held on.
 *
 */
static int readdir_get_data(struct fuse_context *context,
			    struct dir_handle *dir_handle,
			    off_t offset)
{
	struct fs_handle *fs_handle = (struct fs_handle *)context->private_data;
	struct iof_state *iof_state = fs_handle->iof_state;
	struct iof_readdir_in *in = NULL;
	struct readdir_cb_r reply = {0};
	crt_rpc_t *rpc = NULL;

	int ret;
	int rc;

	if (!iof_state) {
		IOF_LOG_ERROR("Could not retrieve iof state");
		return EIO;
	}

	ret = crt_req_create(iof_state->crt_ctx, iof_state->dest_ep,
			     FS_TO_OP(fs_handle, readdir), &rpc);
	if (ret || !rpc) {
		IOF_LOG_ERROR("Could not create request, ret = %d",
			      ret);
		return EIO;
	}

	in = crt_req_get(rpc);
	in->gah = dir_handle->gah;
	in->offsef = offset;
	in->my_fs_id = fs_handle->my_fs_id;

	ret = crt_req_send(rpc, readdir_cb, &reply);
	if (ret) {
		IOF_LOG_ERROR("Could not send rpc, ret = %d", ret);
		return EIO;
	}

	rc = ioc_cb_progress(iof_state->crt_ctx, context, &reply.complete);
	if (rc)
		return rc;

	if (reply.err != 0)
		return reply.err;

	if (reply.out->err != 0) {
		if (reply.out->err == IOF_GAH_INVALID)
			dir_handle->gah_valid = 0;
		IOF_LOG_ERROR("Error from target %d", reply.out->err);
		crt_req_decref(reply.rpc);
		return EIO;
	}

	dir_handle->reply_count = reply.out->replies.iov_len /
		sizeof(struct iof_readdir_reply);
	IOF_LOG_DEBUG("More data received %d %p", dir_handle->reply_count,
		      reply.out->replies.iov_buf);

	if (dir_handle->reply_count != 0) {
		dir_handle->replies = reply.out->replies.iov_buf;
		dir_handle->rpc = reply.rpc;
	} else {
		crt_req_decref(reply.rpc);
		dir_handle->replies = NULL;
		dir_handle->rpc = NULL;
	}

	return 0;
}

/* Mark a previously fetched handle complete */
static void readdir_next_reply_consume(struct dir_handle *dir_handle)
{
	if (dir_handle->reply_count == 0 && dir_handle->rpc) {
		crt_req_decref(dir_handle->rpc);
		dir_handle->rpc = NULL;
	}
}

/* Fetch a pointer to the next reply entry from the target
 *
 * Replies are read from the server in batches, configurable on the server side,
 * the client keeps a array of received but unprocessed replies.  This function
 * returns a new reply if possible, either from the from the front of the local
 * array, or if the array is empty by sending a new RPC.
 *
 * There is no caching on the server, and when the server responds to a RPC it
 * can include zero or more replies.
 */
static int readdir_next_reply(struct fuse_context *context,
			      struct dir_handle *dir_handle,
			      off_t offset,
			      struct iof_readdir_reply **reply)
{
	int rc;

	*reply = NULL;

	/* Check for available data and fetch more if none */
	if (dir_handle->reply_count == 0) {
		IOF_LOG_DEBUG("Fetching more data");
		if (dir_handle->rpc) {
			crt_req_decref(dir_handle->rpc);
			dir_handle->rpc = NULL;
		}
		rc = readdir_get_data(context, dir_handle, offset);
		if (rc != 0) {
			dir_handle->handle_valid = 0;
			return rc;
		}
	}

	if (dir_handle->reply_count == 0) {
		IOF_LOG_DEBUG("No more replies");
		if (dir_handle->rpc) {
			crt_req_decref(dir_handle->rpc);
			dir_handle->rpc = NULL;
		}
		return 0;
	}

	*reply = dir_handle->replies;
	dir_handle->replies++;
	dir_handle->reply_count--;

	return 0;
}

int ioc_readdir(const char *dir, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi
#ifdef IOF_USE_FUSE3
		, enum fuse_readdir_flags flags
#endif
	)
{
	struct fuse_context *context;
	int ret;

	struct dir_handle *dir_handle = (struct dir_handle *)fi->fh;

	IOF_LOG_INFO("path %s %s handle %p", dir, dir_handle->name, dir_handle);

	/* If the handle has been reported as invalid in the past then do not
	 * process any more requests at this stage.
	 */
	if (!dir_handle->handle_valid)
		return -EIO;

	context = fuse_get_context();

	{
		char *d = ios_gah_to_str(&dir_handle->gah);

		IOF_LOG_INFO("Dah %s", d);
		free(d);
	}

	do {
		struct iof_readdir_reply *dir_reply;

		ret = readdir_next_reply(context, dir_handle, offset,
					 &dir_reply);

		IOF_LOG_DEBUG("err %d buf %p", ret, dir_reply);

		if (ret != 0)
			return -ret;

		/* Check for end of directory.  When the end of the directory
		 * stream is reached on the server then we exit here.
		 */
		if (!dir_reply)
			return 0;

		IOF_LOG_DEBUG("reply last %d rc %d stat_rc %d",
			      dir_reply->last, dir_reply->read_rc,
			      dir_reply->stat_rc);

		/* Check for error.  Error on the remote readdir() call exits
		 * here
		 */
		if (dir_reply->read_rc != 0) {
			ret = dir_reply->read_rc;
			readdir_next_reply_consume(dir_handle);
			return -ret;
		}

		if (dir_reply->last != 0) {
			readdir_next_reply_consume(dir_handle);
			IOF_LOG_INFO("Returning no more data");
			return 0;
		}

		/* Process any new information received in this RPC.  The
		 * server will have returned a directory entry name and
		 * possibly a struct stat.
		 *
		 * POSIX: If the directory has been renamed since the opendir()
		 * call and before the readdir() then the remote stat() may
		 * have failed so check for that here.
		 */

		ret = filler(buf, dir_reply->d_name,
			     dir_reply->stat_rc == 0 ? &dir_reply->stat :  NULL,
			     0
#ifdef IOF_USE_FUSE3
			     , 0
#endif
			     );

		IOF_LOG_DEBUG("New file %s %d", dir_reply->d_name, ret);

		/* Check for filler() returning full.  The filler function
		 * returns -1 once the internal FUSE buffer is full so check
		 * for that case and exit the loop here.
		 */
	} while (ret == 0);
	readdir_next_reply_consume(dir_handle);
	IOF_LOG_INFO("Returning zero");
	return 0;
}

