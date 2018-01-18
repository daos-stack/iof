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

	memcpy(&entry.attr, &out->stat, sizeof(struct stat));
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
		IOF_TRACE_INFO(req, "New file %lu " GAH_PRINT_STR,
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
		IOF_TRACE_INFO(req, "Existing file rlink %p %lu "
			       GAH_PRINT_STR, rlink, entry.ino,
			       GAH_PRINT_VAL(out->gah));
		drop_ino_ref(handle->fs_handle, handle->ie->parent);
		ie_close(handle->fs_handle, handle->ie);
	}

	IOF_FUSE_REPLY_CREATE(req, entry, fi);
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

	STAT_ADD(fs_handle->stats, create);

	if (FS_IS_OFFLINE(fs_handle))
		D_GOTO(out_err, ret = fs_handle->offline_reason);

	/* O_LARGEFILE should always be set on 64 bit systems, and in fact is
	 * defined to 0 so IOF defines LARGEFILE to the value that O_LARGEFILE
	 * would otherwise be using and check that is set.
	 */
	if (!(fi->flags & LARGEFILE)) {
		IOF_TRACE_INFO(req, "O_LARGEFILE required 0%o",
			       fi->flags);
		D_GOTO(out_err, ret = ENOTSUP);
	}

	/* Check for flags that do not make sense in this context.
	 */
	if (fi->flags & IOF_UNSUPPORTED_CREATE_FLAGS) {
		IOF_TRACE_INFO(req, "unsupported flag requested 0%o",
			       fi->flags);
		D_GOTO(out_err, ret = ENOTSUP);
	}

	/* Check that only the flag for a regular file is specified */
	if ((mode & S_IFMT) != S_IFREG) {
		IOF_TRACE_INFO(req, "unsupported mode requested 0%o",
			       mode);
		D_GOTO(out_err, ret = ENOTSUP);
	}

	if (!IOF_IS_WRITEABLE(fs_handle->flags)) {
		IOF_TRACE_INFO(req, "Attempt to modify Read-Only File "
			       "System");
		D_GOTO(out_err, ret = EROFS);
	}

	handle = iof_pool_acquire(fs_handle->fh_pool);
	if (!handle)
		D_GOTO(out_err, ret = ENOMEM);

	IOF_TRACE_UP(handle, fs_handle, fs_handle->fh_pool->reg.name);
	IOF_TRACE_UP(req, handle, "create_fuse_req");

	handle->common.projection = &fs_handle->proj;
	handle->open_req = req;

	IOF_TRACE_INFO(req, "file '%s' flags 0%o mode 0%o", name, fi->flags,
		       mode);

	in = crt_req_get(handle->creat_rpc);
	IOF_TRACE_LINK(handle->creat_rpc, req, "create_file_rpc");

	/* Find the GAH of the parent */
	rc = find_gah_ref(fs_handle, parent, &in->common.gah);
	if (rc != 0)
		D_GOTO(out_err, ret = ENOENT);

	strncpy(in->common.name.name, name, NAME_MAX);
	in->mode = mode;
	in->flags = fi->flags;

	strncpy(handle->ie->name, name, NAME_MAX);
	handle->ie->parent = parent;

	crt_req_addref(handle->creat_rpc);
	rc = crt_req_send(handle->creat_rpc, ioc_create_ll_cb, handle);
	if (rc) {
		crt_req_decref(handle->creat_rpc);
		drop_ino_ref(fs_handle, parent);
		D_GOTO(out_err, ret = EIO);
	}

	iof_pool_restock(fs_handle->fh_pool);

	LOG_FLAGS(handle, fi->flags);
	LOG_MODES(handle, mode);

	return;
out_err:
	IOF_FUSE_REPLY_ERR(req, ret);

	if (handle)
		iof_pool_release(fs_handle->fh_pool, handle);
}
