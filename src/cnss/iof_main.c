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

#include <inttypes.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <libgen.h>
#ifdef IOF_USE_FUSE3
#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>
#else
#include <fuse/fuse.h>
#include <fuse/fuse_lowlevel.h>
#endif

#include "iof_common.h"
#include "iof.h"
#include "log.h"
#include "ios_gah.h"

/* Data which is stored against an open directory handle */
struct dir_handle {
	/* The name of the directory */
	char *name;
	/* The handle for accessing the directory on the IONSS */
	struct ios_gah gah;
	/* Any RPC reference held across readdir() calls */
	crt_rpc_t *rpc;
	/* Pointer to any retreived data from readdir() RPCs */
	struct iof_readdir_reply *replies;
	int reply_count;
	/* Set to 1 initially, but 0 if there is a unrecoverable error */
	int handle_valid;
	/* Set to 0 if the server rejects the GAH at any point */
	int gah_valid;
};

struct opendir_cb_r {
	struct dir_handle *dh;
	int complete;
	int err;
	int rc;
};

struct readdir_cb_r {
	int complete;
	crt_rpc_t *rpc;
	int err;
	struct iof_readdir_out *out;
};

struct closedir_cb_r {
	int complete;
};

struct query_cb_r {
	int complete;
	struct iof_psr_query **query;
};

static int string_to_bool(const char *str, int *value)
{
	if (strcmp(str, "1") == 0) {
		*value = 1;
		return 1;
	} else if (strcmp(str, "0") == 0) {
		*value = 0;
		return 1;
	}
	return 0;
}

/* on-demand progress */
static int iof_progress(crt_context_t crt_ctx, int num_retries,
			unsigned int wait_len_ms, int *complete_flag)
{
	int		retry;
	int		rc;

	for (retry = 0; retry < num_retries; retry++) {
		rc = crt_progress(crt_ctx, wait_len_ms * 1000, NULL, NULL);
		if (rc != 0 && rc != -CER_TIMEDOUT) {
			IOF_LOG_ERROR("crt_progress failed rc: %d", rc);
			break;
		}
		if (*complete_flag)
			return 0;
		sched_yield();
	}
	return -ETIMEDOUT;
}

/* Progress, from within FUSE callbacks during normal I/O
 *
 * This will use a default set of values to iof_progress, and do the correct
 * thing on timeout, potentially shutting dowth the filesystem if there is
 * a problem.
 */
int ioc_cb_progress(crt_context_t crt_ctx, struct fuse_context *context,
		    int *complete_flag)
{
	int rc;

	rc = iof_progress(crt_ctx, 50, 6000, complete_flag);
	if (rc) {
		/*TODO: check is PSR is alive before exiting fuse*/
		IOF_LOG_ERROR("exiting fuse loop");
		fuse_session_exit(fuse_get_session(context->fuse));
		return EINTR;
	}
	return 0;
}

#if IOF_USE_FUSE3
static int ioc_getattr3(const char *path, struct stat *stbuf,
			struct fuse_file_info *fi)
{
	if (fi)
		return -EIO;

	if (!path)
		return -EIO;
	return ioc_getattr(path, stbuf);
}
#endif

/*
 * Temporary way of shutting down. This fuse callback is currently not going to
 * IONSS
 */
#ifdef __APPLE__

static int ioc_setxattr(const char *path,  const char *name, const char *value,
			size_t size, int options, uint32_t position)
#else
static int ioc_setxattr(const char *path, const char *name, const char *value,
			size_t size, int flags)
#endif
{
	struct fuse_context *context;
	int ret;

	context = fuse_get_context();
	if (strcmp(name, "user.exit") == 0) {
		if (string_to_bool(value, &ret)) {
			if (ret) {
				IOF_LOG_INFO("Exiting fuse loop");
				fuse_session_exit(
					fuse_get_session(context->fuse));
				return -EINTR;
			} else
				return 0;
		}
		return -EINVAL;
	}
	return -ENOTSUP;
}

static int opendir_cb(const struct crt_cb_info *cb_info)
{
	struct opendir_cb_r *reply = NULL;
	struct iof_opendir_out *out = NULL;
	crt_rpc_t *rpc = cb_info->cci_rpc;

	reply = (struct opendir_cb_r *)cb_info->cci_arg;

	if (cb_info->cci_rc != 0) {
		/*
		 * Error handling.  On timeout return EAGAIN, all other errors
		 * return EIO.
		 *
		 * TODO: Handle target eviction here
		 */
		IOF_LOG_INFO("Bad RPC reply %d", cb_info->cci_rc);
		if (cb_info->cci_rc == -CER_TIMEDOUT)
			reply->rc = EAGAIN;
		else
			reply->rc = EIO;
		reply->complete = 1;
		return 0;
	}

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not get opendir output");
		reply->rc = EIO;
		reply->complete = 1;
		return 0;
	}
	if (out->err == 0 && out->rc == 0) {
		memcpy(&reply->dh->gah, out->gah.iov_buf,
		       sizeof(struct ios_gah));
		reply->dh->gah_valid = 1;
		reply->dh->handle_valid = 1;
	}
	reply->err = out->err;
	reply->rc = out->rc;
	reply->complete = 1;
	return 0;
}

static int ioc_opendir(const char *dir, struct fuse_file_info *fi)
{
	struct fuse_context *context;
	struct dir_handle *dir_handle;
	uint64_t ret;
	struct iof_string_in *in = NULL;
	struct opendir_cb_r reply = {0};
	struct fs_handle *fs_handle;
	struct iof_state *iof_state = NULL;
	crt_rpc_t *rpc = NULL;
	int rc;

	dir_handle = calloc(1, sizeof(struct dir_handle));
	if (!dir_handle)
		return -EIO;
	dir_handle->name = strdup(dir);
	if (!dir_handle->name) {
		free(dir_handle);
		return -EIO;
	}

	IOF_LOG_INFO("dir %s handle %p", dir, dir_handle);

	context = fuse_get_context();
	fs_handle = (struct fs_handle *)context->private_data;
	iof_state = fs_handle->iof_state;
	if (!iof_state) {
		IOF_LOG_ERROR("Could not retrieve iof state");
		free(dir_handle->name);
		free(dir_handle);
		return -EIO;
	}

	ret = crt_req_create(iof_state->crt_ctx, iof_state->dest_ep, OPENDIR_OP,
			     &rpc);
	if (ret || !rpc) {
		IOF_LOG_ERROR("Could not create opendir request, ret = %lu",
			      ret);
		free(dir_handle->name);
		free(dir_handle);
		return -EIO;
	}

	in = crt_req_get(rpc);
	in->path = (crt_string_t)dir;
	in->my_fs_id = (uint64_t)fs_handle->my_fs_id;

	reply.dh = dir_handle;
	reply.complete = 0;

	ret = crt_req_send(rpc, opendir_cb, &reply);
	if (ret) {
		IOF_LOG_ERROR("Could not send opendir rpc, ret = %lu", ret);
		free(dir_handle->name);
		free(dir_handle);
		return -EIO;
	}

	rc = ioc_cb_progress(iof_state->crt_ctx, context, &reply.complete);
	if (rc) {
		free(dir_handle->name);
		free(dir_handle);
		return -rc;
	}

	if (reply.err == 0 && reply.rc == 0) {
		char *d = ios_gah_to_str(&dir_handle->gah);

		fi->fh = (uint64_t)dir_handle;

		IOF_LOG_INFO("Dah %s", d);
		free(d);
	} else {
		free(dir_handle->name);
		free(dir_handle);
	}

	IOF_LOG_DEBUG("path %s rc %d",
		      dir, reply.err == 0 ? -reply.rc : -EIO);

	return reply.err == 0 ? -reply.rc : -EIO;
}

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
			     READDIR_OP, &rpc);
	if (ret || !rpc) {
		IOF_LOG_ERROR("Could not create request, ret = %d",
			      ret);
		return EIO;
	}

	in = crt_req_get(rpc);
	crt_iov_set(&in->gah, &dir_handle->gah, sizeof(struct ios_gah));
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

static int
ioc_readdir(const char *dir, void *buf, fuse_fill_dir_t filler,
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

static int closedir_cb(const struct crt_cb_info *cb_info)
{
	struct closedir_cb_r *reply = (struct closedir_cb_r *)cb_info->cci_arg;

	/* There is no error handling needed here, as all client state will be
	 * destroyed on return anyway.
	 */

	reply->complete = 1;
	return 0;
}

static int ioc_closedir(const char *dir, struct fuse_file_info *fi)
{
	struct fuse_context *context;
	uint64_t ret;
	struct iof_closedir_in *in = NULL;
	struct closedir_cb_r reply = {0};
	struct fs_handle *fs_handle;
	struct iof_state *iof_state = NULL;
	crt_rpc_t *rpc = NULL;
	int rc = 0;

	struct dir_handle *dir_handle = (struct dir_handle *)fi->fh;

	IOF_LOG_INFO("path %s %s handle %p", dir, dir_handle->name, dir_handle);

	/* If the GAH has been reported as invalid by the server in the past
	 * then do not attempt to do anything with it.
	 *
	 * However, even if the local handle has been reported invalid then
	 * still continue to release the GAH on the server side.
	 */
	if (!dir_handle->gah_valid) {
		rc = EIO;
		goto out;
	}

	context = fuse_get_context();
	fs_handle = (struct fs_handle *)context->private_data;
	iof_state = fs_handle->iof_state;
	if (!iof_state) {
		IOF_LOG_ERROR("Could not retrieve iof state");
		rc = EIO;
		goto out;
	}

	ret = crt_req_create(iof_state->crt_ctx, iof_state->dest_ep,
			     CLOSEDIR_OP, &rpc);
	if (ret || !rpc) {
		IOF_LOG_ERROR("Could not create closedir request, ret = %lu",
			      ret);
		rc = EIO;
		goto out;
	}

	in = crt_req_get(rpc);
	crt_iov_set(&in->gah, &dir_handle->gah, sizeof(struct ios_gah));

	ret = crt_req_send(rpc, closedir_cb, &reply);
	if (ret) {
		IOF_LOG_ERROR("Could not send closedir rpc, ret = %lu", ret);
		rc = EIO;
		goto out;
	}

	{
		char *d = ios_gah_to_str(&dir_handle->gah);

		IOF_LOG_INFO("Dah %s", d);
		free(d);
	}

	rc = ioc_cb_progress(iof_state->crt_ctx, context, &reply.complete);

out:
	/* If there has been an error on the local handle, or readdir() is not
	 * exhausted then ensure that all resources are freed correctly
	 */
	if (dir_handle->rpc)
		crt_req_decref(dir_handle->rpc);

	if (dir_handle->name)
		free(dir_handle->name);
	free(dir_handle);
	return -rc;
}

static struct fuse_operations ops = {
#if IOF_USE_FUSE3
	.getattr = ioc_getattr3,
#else
	.flag_nopath = 1,
	.getattr = ioc_getattr,
#endif
	.opendir = ioc_opendir,
	.readdir = ioc_readdir,
	.releasedir = ioc_closedir,
	.setxattr = ioc_setxattr,
	.open = ioc_open,
	.release = ioc_release,
	.create = ioc_create,
};

static int query_callback(const struct crt_cb_info *cb_info)
{
	struct query_cb_r *reply;
	int ret;
	crt_rpc_t *query_rpc;

	query_rpc = cb_info->cci_rpc;
	reply = (struct query_cb_r *) cb_info->cci_arg;

	*reply->query = crt_reply_get(query_rpc);
	if (*reply->query == NULL) {
		IOF_LOG_ERROR("Could not get query reply");
		return IOF_ERR_CART;
	}

	ret = crt_req_addref(query_rpc);
	if (ret)
		IOF_LOG_ERROR("could not take reference on query RPC, ret = %d",
				ret);
	reply->complete = 1;
	return ret;
}

/*Send RPC to PSR to get information about projected filesystems*/
static int ioc_get_projection_info(struct iof_state *iof_state,
				   struct iof_psr_query **query,
				   crt_rpc_t **query_rpc)
{
	int ret;
	struct query_cb_r reply = {0};

	reply.complete = 0;
	reply.query = query;

	ret = crt_req_create(iof_state->crt_ctx, iof_state->dest_ep,
				QUERY_PSR_OP, query_rpc);
	if (ret || (*query_rpc == NULL)) {
		IOF_LOG_ERROR("failed to create query rpc request, ret = %d",
				ret);
		return ret;
	}

	ret = crt_req_send(*query_rpc, query_callback, &reply);
	if (ret) {
		IOF_LOG_ERROR("Could not send query RPC, ret = %d", ret);
		return ret;
	}

	/*make on-demand progress*/
	ret = iof_progress(iof_state->crt_ctx, 50, 6000, &reply.complete);
	if (ret)
		IOF_LOG_ERROR("Could not complete PSR query");

	return ret;
}


int iof_reg(void *foo, struct cnss_plugin_cb *cb,
	    size_t cb_size)
{
	struct iof_handle *handle = (struct iof_handle *)foo;
	struct iof_state *iof_state;
	crt_group_t *ionss_group;
	char *prefix;
	int ret;
	DIR *prefix_dir;

	/* First check for the IONSS process set, and if it does not exist then
	 * return cleanly to allow the rest of the CNSS code to run
	 */
	ret = crt_group_attach("IONSS", &ionss_group);
	if (ret) {
		IOF_LOG_INFO("crt_group_attach failed with ret = %d", ret);
		return ret;
	}

	if (!handle->state) {
		handle->state = calloc(1, sizeof(struct iof_state));
		if (!handle->state)
			return IOF_ERR_NOMEM;
	}

	/*initialize iof state*/
	iof_state = handle->state;
	/*do a group lookup*/
	iof_state->dest_group = ionss_group;

	/*initialize destination endpoint*/
	iof_state->dest_ep.ep_grp = 0; /*primary group*/
	/*TODO: Use exported PSR from cart*/
	iof_state->dest_ep.ep_rank = 0;
	iof_state->dest_ep.ep_tag = 0;

	ret = crt_context_create(NULL, &iof_state->crt_ctx);
	if (ret)
		IOF_LOG_ERROR("Context not created");
	prefix = getenv("CNSS_PREFIX");
	iof_state->cnss_prefix = realpath(prefix, NULL);
	prefix_dir = opendir(iof_state->cnss_prefix);
	if (prefix_dir)
		closedir(prefix_dir);
	else {
		if (mkdir(iof_state->cnss_prefix, 0755)) {
			IOF_LOG_ERROR("Could not create cnss_prefix");
			return CNSS_ERR_PREFIX;
		}
	}

	/*registrations*/
	ret = crt_rpc_register(QUERY_PSR_OP, &QUERY_RPC_FMT);
	if (ret) {
		IOF_LOG_ERROR("Query rpc registration failed with ret: %d",
				ret);
		return ret;
	}

	ret = crt_rpc_register(GETATTR_OP, &GETATTR_FMT);
	if (ret) {
		IOF_LOG_ERROR("getattr registration failed with ret: %d", ret);
		return ret;
	}

	ret = crt_rpc_register(SHUTDOWN_OP, NULL);
	if (ret) {
		IOF_LOG_ERROR("shutdown registration failed with ret: %d", ret);
		return ret;
	}

	ret = crt_rpc_register(OPENDIR_OP, &OPENDIR_FMT);
	if (ret) {
		IOF_LOG_ERROR("Can not register opendir RPC, ret = %d", ret);
		return ret;
	}

	ret = crt_rpc_register(READDIR_OP, &READDIR_FMT);
	if (ret) {
		IOF_LOG_ERROR("Can not register readdir RPC, ret = %d", ret);
		return ret;
	}

	ret = crt_rpc_register(CLOSEDIR_OP, &CLOSEDIR_FMT);
	if (ret) {
		IOF_LOG_ERROR("Can not register closedir RPC, ret = %d", ret);
		return ret;
	}

	ret = crt_rpc_register(OPEN_OP, &OPEN_FMT);
	if (ret) {
		IOF_LOG_ERROR("Can not register open RPC, ret = %d", ret);
		return ret;
	}

	ret = crt_rpc_register(CLOSE_OP, &CLOSE_FMT);
	if (ret) {
		IOF_LOG_ERROR("Can not register close RPC, ret = %d", ret);
		return ret;
	}

	ret = crt_rpc_register(CREATE_OP, &CREATE_FMT);
	if (ret) {
		IOF_LOG_ERROR("Can not register create RPC, ret = %d", ret);
		return ret;
	}

	handle->cb = cb;

	return ret;
}

int iof_post_start(void *foo)
{
	struct iof_handle *iof_handle = (struct iof_handle *)foo;
	struct iof_psr_query *query = NULL;
	int ret;
	int i;
	int fs_num;
	char mount[IOF_NAME_LEN_MAX];
	char ctrl_path[IOF_NAME_LEN_MAX];
	char base_mount[IOF_NAME_LEN_MAX];
	struct cnss_plugin_cb *cb;
	struct iof_state *iof_state = NULL;
	struct fs_handle *fs_handle = NULL;
	struct iof_fs_info *tmp;
	crt_rpc_t *query_rpc = NULL;

	ret = IOF_SUCCESS;
	iof_state = iof_handle->state;
	cb = iof_handle->cb;


	/*Query PSR*/
	ret = ioc_get_projection_info(iof_state, &query, &query_rpc);
	if (ret || (query == NULL)) {
		IOF_LOG_ERROR("Query operation failed");
		return IOF_ERR_PROJECTION;
	}

	/*calculate number of projections*/
	fs_num = (query->query_list.iov_len)/sizeof(struct iof_fs_info);
	IOF_LOG_DEBUG("Number of filesystems projected: %d", fs_num);

	tmp = (struct iof_fs_info *) query->query_list.iov_buf;

	strncpy(base_mount, iof_state->cnss_prefix, IOF_NAME_LEN_MAX);
	for (i = 0; i < fs_num; i++) {
		char *base_name;

		if (tmp[i].mode == 0) {
			fs_handle = calloc(1, sizeof(struct fs_handle));
			if (!fs_handle)
				return IOF_ERR_NOMEM;
			fs_handle->iof_state = iof_state;
			IOF_LOG_INFO("Filesystem mode: Private");

		} else
			return IOF_NOT_SUPP;


		snprintf(ctrl_path, IOF_NAME_LEN_MAX, "/iof/PA/mount%d", i);
		IOF_LOG_DEBUG("Ctrl path is: %s%s", iof_state->cnss_prefix,
								ctrl_path);

		base_name = basename(tmp[i].mnt);

		IOF_LOG_DEBUG("Projected Mount %s", base_name);
		snprintf(mount, IOF_NAME_LEN_MAX, "%s/%s", base_mount,
								base_name);
		IOF_LOG_INFO("Mountpoint for this projection: %s", mount);

		/*Register the mount point with the control filesystem*/
		cb->register_ctrl_constant(ctrl_path, mount);

		fs_handle->my_fs_id = tmp[i].id;
		IOF_LOG_INFO("Filesystem ID %" PRIu64, tmp[i].id);
		if (cb->register_fuse_fs(cb->handle, &ops, mount, fs_handle)
								!= NULL)
			IOF_LOG_DEBUG("Fuse mount installed at: %s", mount);

	}

	ret = crt_req_decref(query_rpc);
	if (ret)
		IOF_LOG_ERROR("Could not decrement ref count on query rpc");
	return ret;
}

void iof_flush(void *handle)
{

	IOF_LOG_INFO("Called iof_flush");

}

static int shutdown_cb(const struct crt_cb_info *cb_info)
{
	int *complete;

	complete = (int *)cb_info->cci_arg;

	*complete = 1;

	return IOF_SUCCESS;
}

void iof_finish(void *handle)
{
	int ret;
	crt_rpc_t *shut_rpc;
	int complete;
	struct iof_handle *iof_handle = (struct iof_handle *)handle;
	struct iof_state *iof_state = iof_handle->state;

	/*send a detach RPC to IONSS*/
	ret = crt_req_create(iof_state->crt_ctx, iof_state->dest_ep,
			SHUTDOWN_OP, &shut_rpc);
	if (ret)
		IOF_LOG_ERROR("Could not create shutdown request ret = %d",
				ret);

	complete = 0;
	ret = crt_req_send(shut_rpc, shutdown_cb, &complete);
	if (ret)
		IOF_LOG_ERROR("shutdown RPC not sent");

	ret = iof_progress(iof_state->crt_ctx, 50, 6000, &complete);
	if (ret)
		IOF_LOG_ERROR("Could not progress shutdown RPC");
	ret = crt_context_destroy(iof_state->crt_ctx, 0);
	if (ret)
		IOF_LOG_ERROR("Could not destroy context");
	IOF_LOG_INFO("Called iof_finish with %p", handle);

	ret = crt_group_detach(iof_state->dest_group);
	if (ret)
		IOF_LOG_ERROR("crt_group_detach failed with ret = %d", ret);

	free(iof_state->cnss_prefix);
	free(iof_state);
	free(iof_handle);
}

struct cnss_plugin self = {.name            = "iof",
			   .version         = CNSS_PLUGIN_VERSION,
			   .require_service = 0,
			   .start           = iof_reg,
			   .post_start      = iof_post_start,
			   .flush           = iof_flush,
			   .finish          = iof_finish};

int iof_plugin_init(struct cnss_plugin **fns, size_t *size)
{
	*size = sizeof(struct cnss_plugin);

	self.handle = calloc(1, sizeof(struct iof_handle));
	if (!self.handle)
		return IOF_ERR_NOMEM;
	*fns = &self;
	return IOF_SUCCESS;
}

