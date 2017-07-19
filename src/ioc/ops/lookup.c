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

#include <fuse3/fuse.h>
#include "iof_common.h"
#include "ioc.h"
#include "log.h"

static void
lookup_cb(const struct crt_cb_info *cb_info)
{
	struct lookup_req	*desc = cb_info->cci_arg;
	struct iof_lookup_out	*out = crt_reply_get(cb_info->cci_rpc);
	struct fuse_entry_param	 entry = {0};
	d_list_t		*rlink;
	int			rc = EIO;

	IOF_LOG_INFO("cb, reply %d", cb_info->cci_rc);
	if (IOC_HOST_IS_DOWN(cb_info)) {
		rc = EHOSTDOWN;
		goto out;
	}

	if (cb_info->cci_rc != 0)
		goto out;

	if (out->rc != 0) {
		rc = out->rc;
		goto out;
	}

	if (out->err != 0)
		goto out;

	if (!out->stat.iov_buf)
		goto out;

	memcpy(&entry.attr, out->stat.iov_buf, sizeof(struct stat));
	entry.generation = 1;
	entry.ino = entry.attr.st_ino;

	desc->ie->ino = entry.attr.st_ino;
	desc->ie->gah = out->gah;
	rlink = d_chash_rec_find_insert(&desc->fs_handle->inode_ht,
					&entry.ino,
					sizeof(entry.ino),
					&desc->ie->list);

	if (rlink == &desc->ie->list) {
		desc->ie = NULL;
		IOF_LOG_INFO("New file rlink %p %lu " GAH_PRINT_STR,
			     rlink, entry.ino, GAH_PRINT_VAL(out->gah));
	} else {
		/* TODO: Free the GAH */
		IOF_LOG_INFO("Existing file rlink %p %lu " GAH_PRINT_STR,
			     rlink, entry.ino, GAH_PRINT_VAL(out->gah));
	}

	fuse_reply_entry(desc->req, &entry);
	iof_pool_release(desc->fs_handle->lookup_pool, desc);
	return;

out:
	fuse_reply_err(desc->req, rc);
	iof_pool_release(desc->fs_handle->lookup_pool, desc);
}

void
ioc_ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct iof_projection_info	*fs_handle = fuse_req_userdata(req);
	struct lookup_req		*desc;
	struct iof_gah_string_in	*in;
	int rc;

	IOF_LOG_INFO("Req %p %lu %s", req, parent, name);

	STAT_ADD(fs_handle->stats, lookup);

	if (FS_IS_OFFLINE(fs_handle)) {
		fuse_reply_err(req, fs_handle->offline_reason);
		return;
	}

	desc = iof_pool_acquire(fs_handle->lookup_pool);
	if (!desc) {
		fuse_reply_err(req, ENOMEM);
		return;
	}

	IOF_LOG_INFO("Req %p ie %p", req, &desc->ie->list);

	in = crt_req_get(desc->rpc);
	in->path = (d_string_t)name;

	/* Find the GAH of the parent */
	rc = find_gah(fs_handle, parent, &in->gah);
	if (rc != 0) {
		iof_pool_release(fs_handle->lookup_pool, desc);
		fuse_reply_err(req, ENOENT);
	}

	desc->req = req;

	rc = crt_req_set_endpoint(desc->rpc, &fs_handle->proj.grp->psr_ep);
	if (rc) {
		IOF_LOG_ERROR("Could not rpc endpoint, rc = %d", rc);
		fuse_reply_err(req, EIO);
		iof_pool_release(fs_handle->lookup_pool, desc);
		return;
	}

	rc = crt_req_send(desc->rpc, lookup_cb, desc);
	if (rc) {
		IOF_LOG_ERROR("Could not send rpc, rc = %d", rc);
		fuse_reply_err(req, EIO);
		iof_pool_release(fs_handle->lookup_pool, desc);
		return;
	}
	crt_req_addref(desc->rpc);
	iof_pool_restock(fs_handle->lookup_pool);
}
