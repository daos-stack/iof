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

void
ioc_create_cb(const struct crt_cb_info *cb_info)
{
	struct open_cb_r *reply = cb_info->cci_arg;
	struct iof_create_out	*out = crt_reply_get(cb_info->cci_rpc);

	if (cb_info->cci_rc != 0) {
		/*
		 * Error handling.  On timeout return EAGAIN, all other errors
		 * return EIO.
		 *
		 * TODO: Handle target eviction here
		 */
		IOF_TRACE_INFO(reply->fh, "Bad RPC reply %d", cb_info->cci_rc);
		if (cb_info->cci_rc == -DER_TIMEDOUT)
			reply->err = EAGAIN;
		else
			reply->err = EIO;
		iof_tracker_signal(&reply->tracker);
		return;
	}

	if (out->err == 0 && out->rc == 0)
		reply->fh->common.gah = out->gah;
	reply->err = out->err;
	reply->rc = out->rc;
	iof_tracker_signal(&reply->tracker);
}

int ioc_create(const char *file, mode_t mode, struct fuse_file_info *fi)
{
	struct iof_projection_info *fs_handle = ioc_get_handle();
	struct iof_file_handle *handle;
	struct iof_create_in *in;
	struct open_cb_r reply = {0};
	int rc;

	STAT_ADD(fs_handle->stats, create);

	if (FS_IS_OFFLINE(fs_handle))
		return -fs_handle->offline_reason;

	if (strnlen(file, NAME_MAX) == NAME_MAX)
		return -EIO;

	/* O_LARGEFILE should always be set on 64 bit systems, and in fact is
	 * defined to 0 so check that LARGEFILE is set and reject the open
	 * if not.
	 */
	if (!(fi->flags & LARGEFILE)) {
		IOF_TRACE_INFO(fs_handle, "O_LARGEFILE required 0%o",
			       fi->flags);
		return -ENOTSUP;
	}

	/* Check that a regular file is requested */
	if (!(mode & S_IFREG)) {
		IOF_TRACE_INFO(fs_handle, "S_IFREG required 0%o", fi->flags);
		return -ENOTSUP;
	}

	/* Check for flags that do not make sense in this context.
	 */
	if (fi->flags & IOF_UNSUPPORTED_CREATE_FLAGS) {
		IOF_TRACE_INFO(fs_handle, "unsupported flag requested 0%o",
			       fi->flags);
		return -ENOTSUP;
	}

	/* Check that only the flag for a regular file is specified */
	if ((mode & S_IFMT) != S_IFREG) {
		IOF_TRACE_INFO(fs_handle, "unsupported mode requested 0%o",
			       mode);
		return -ENOTSUP;
	}

	if (!IOF_IS_WRITEABLE(fs_handle->flags)) {
		IOF_TRACE_INFO(fs_handle, "Attempt to modify Read-Only File "
			       "System");
		return -EROFS;
	}

	handle = iof_pool_acquire(fs_handle->fh_pool);
	if (!handle)
		return -ENOMEM;

	handle->common.projection = &fs_handle->proj;

	IOF_TRACE_INFO(handle, "file %s flags 0%o mode 0%o", file, fi->flags,
		       mode);

	iof_tracker_init(&reply.tracker, 1);
	in = crt_req_get(handle->creat_rpc);
	strncpy(in->common.name.name, file, NAME_MAX);
	in->mode = mode;
	in->common.gah = fs_handle->gah;
	in->flags = fi->flags;

	reply.fh = handle;

	rc = crt_req_send(handle->creat_rpc, ioc_create_cb, &reply);
	if (rc) {
		IOF_TRACE_ERROR(handle, "Could not send rpc, rc = %d", rc);
		iof_pool_release(fs_handle->fh_pool, handle);
		return -EIO;
	}
	handle->name = strdup(file);
	iof_pool_restock(fs_handle->fh_pool);
	crt_req_addref(handle->creat_rpc);

	LOG_FLAGS(handle, fi->flags);
	LOG_MODES(handle, mode);

	iof_fs_wait(&fs_handle->proj, &reply.tracker);

	if (reply.err == 0 && reply.rc == 0)
		IOF_TRACE_INFO(handle, " " GAH_PRINT_STR,
			       GAH_PRINT_VAL(handle->common.gah));

	rc = reply.err == 0 ? -reply.rc : -EIO;

	IOF_TRACE_DEBUG(handle, "path %s handle %p rc %d",
			handle->name, rc == 0 ? handle : NULL, rc);

	if (rc == 0) {
		fi->fh = (uint64_t)handle;
		handle->common.gah_valid = 1;
	} else {
		iof_pool_release(fs_handle->fh_pool, handle);
	}

	return rc;
}

static void
ioc_create_ll_cb(const struct crt_cb_info *cb_info)
{
	struct iof_file_handle	*handle = cb_info->cci_arg;
	struct iof_create_out	*out = crt_reply_get(cb_info->cci_rpc);
	struct fuse_file_info	fi = {0};
	struct fuse_entry_param entry = {0};
	d_list_t		*rlink;
	fuse_req_t		req;
	int			ret = EIO;

	IOF_TRACE_DEBUG(handle, "cci_rc %d rc %d err %d",
			cb_info->cci_rc, out->rc, out->err);

	if (cb_info->cci_rc != 0)
		D_GOTO(out_err, ret = EIO);

	if (out->rc)
		D_GOTO(out_err, ret = out->rc);

	if (out->err)
		D_GOTO(out_err, ret = EIO);

	/* Create a new FI descriptor from the RPC reply */

	/* Reply to the create request with the GAH from the create call
	 */
	fi.fh = (uint64_t)handle;
	handle->common.gah = out->gah;
	handle->common.gah_valid = 1;
	req = handle->open_req;
	handle->open_req = 0;

	memcpy(&entry.attr, out->stat.iov_buf, sizeof(struct stat));
	entry.generation = 1;
	entry.ino = entry.attr.st_ino;

	/* Populate the inode table with the GAH from the duplicate file
	 * so that it can still be accessed after the file is closed
	 */
	handle->ie->ino = entry.attr.st_ino;
	handle->ie->gah = out->igah;
	rlink = d_chash_rec_find_insert(&handle->fs_handle->inode_ht,
					&entry.ino,
					sizeof(entry.ino),
					&handle->ie->list);

	if (rlink == &handle->ie->list) {
		handle->ie = NULL;
		IOF_TRACE_INFO(handle, "New file %lu " GAH_PRINT_STR,
			       entry.ino, GAH_PRINT_VAL(out->gah));
	} else {
		/* This is an interesting, but not impossible case, although it
		 * could also represent a problem.
		 *
		 * One way of getting here would be to have another thread, with
		 * another RPC looking up the new file, and for the create RPC
		 * to create the file but the lookup RPC to observe the new file
		 * and the reply to arrive first.  Unlikely but possible.
		 *
		 * Another means of getting here would be if the filesystem was
		 * rapidly recycling inodes, and the local entry in cache was
		 * from an old generation.  This in theory should not happen
		 * as an entry in the hash table would mean the server held open
		 * the file, so even if it had been unlinked it would still
		 * exist and thus the inode was unlikely to be reused.
		 */
		IOF_TRACE_INFO(handle, "Existing file rlink %p %lu "
			       GAH_PRINT_STR, rlink, entry.ino,
			       GAH_PRINT_VAL(out->gah));
		drop_ino_ref(handle->fs_handle, handle->ie->parent);
		ie_close(handle->fs_handle, handle->ie);
	}

	fuse_reply_create(req, &entry, &fi);
	IOF_TRACE_DOWN(req);
	return;

out_err:
	IOF_FUSE_REPLY_ERR(handle->open_req, ret);
	drop_ino_ref(handle->fs_handle, handle->ie->parent);
	iof_pool_release(handle->fs_handle->fh_pool, handle);
}

void ioc_ll_create(fuse_req_t req, fuse_ino_t parent, const char *name,
		   mode_t mode, struct fuse_file_info *fi)
{
	struct iof_projection_info *fs_handle = fuse_req_userdata(req);
	struct iof_file_handle *handle = NULL;
	struct iof_create_in *in;
	int ret;
	int rc;

	IOF_TRACE_UP(req, fs_handle, "create");
	STAT_ADD(fs_handle->stats, create);

	if (FS_IS_OFFLINE(fs_handle))
		D_GOTO(out_err, ret = fs_handle->offline_reason);

	/* O_LARGEFILE should always be set on 64 bit systems, and in fact is
	 * defined to 0 so IOF defines LARGEFILE to the value that O_LARGEFILE
	 * would otherwise be using and check that is set.
	 */
	if (!(fi->flags & LARGEFILE)) {
		IOF_TRACE_INFO(fs_handle, "O_LARGEFILE required 0%o",
			       fi->flags);
		D_GOTO(out_err, ret = ENOTSUP);
	}

	/* Check for flags that do not make sense in this context.
	 */
	if (fi->flags & IOF_UNSUPPORTED_CREATE_FLAGS) {
		IOF_TRACE_INFO(fs_handle, "unsupported flag requested 0%o",
			       fi->flags);
		D_GOTO(out_err, ret = ENOTSUP);
	}

	/* Check that only the flag for a regular file is specified */
	if ((mode & S_IFMT) != S_IFREG) {
		IOF_TRACE_INFO(fs_handle, "unsupported mode requested 0%o",
			       mode);
		D_GOTO(out_err, ret = ENOTSUP);
	}

	if (!IOF_IS_WRITEABLE(fs_handle->flags)) {
		IOF_TRACE_INFO(fs_handle, "Attempt to modify Read-Only File "
			       "System");
		D_GOTO(out_err, ret = EROFS);
	}

	handle = iof_pool_acquire(fs_handle->fh_pool);
	if (!handle)
		D_GOTO(out_err, ret = ENOMEM);

	IOF_TRACE_LINK(handle, req, "handle");

	handle->common.projection = &fs_handle->proj;
	handle->open_req = req;

	IOF_TRACE_INFO(handle, "file '%s' flags 0%o mode 0%o", name, fi->flags,
		       mode);

	in = crt_req_get(handle->creat_rpc);

	/* Find the GAH of the parent */
	rc = find_gah_ref(fs_handle, parent, &in->common.gah);
	if (rc != 0)
		D_GOTO(out_err, ret = ENOENT);

	strncpy(in->common.name.name, name, NAME_MAX);
	in->mode = mode;
	in->flags = fi->flags;
	in->reg_inode = 1;

	strncpy(handle->ie->name, name, NAME_MAX);
	handle->ie->parent = parent;

	rc = crt_req_send(handle->creat_rpc, ioc_create_ll_cb, handle);
	if (rc) {
		IOF_TRACE_ERROR(handle, "Could not send rpc, rc = %d", rc);
		drop_ino_ref(fs_handle, parent);
		D_GOTO(out_err, ret = EIO);
	}

	iof_pool_restock(fs_handle->fh_pool);
	crt_req_addref(handle->creat_rpc);

	LOG_FLAGS(handle, fi->flags);
	LOG_MODES(handle, mode);

	return;
out_err:
	IOF_FUSE_REPLY_ERR(req, ret);

	if (handle)
		iof_pool_release(fs_handle->fh_pool, handle);
}
