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

#define REQ_NAME request
#define POOL_NAME lookup_pool
#define TYPE_NAME lookup_req
#define RESTOCK_ON_SEND
#include "ioc_ops.h"

static void lookup_cb(struct ioc_request *request)
{
	struct TYPE_NAME	*desc = CONTAINER(request);
	struct iof_lookup_out	*out = IOC_GET_RESULT(request);
	struct fuse_entry_param	 entry = {0};
	d_list_t		 *rlink;
	int			 rc;

	IOC_RESOLVE_STATUS(request, out);
	rc = IOC_STATUS_TO_RC_LL(request);
	if (rc)
		D_GOTO(out, rc);

	if (!out->stat.iov_buf || out->stat.iov_len != sizeof(struct stat)) {
		IOF_TRACE_ERROR(desc, "stat buff invalid %p %zi",
				out->stat.iov_buf, out->stat.iov_len);
		D_GOTO(out, rc = EIO);
	}

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
		IOF_TRACE_INFO(desc, "New file %lu " GAH_PRINT_STR,
			       entry.ino, GAH_PRINT_VAL(out->gah));
	} else {
		IOF_TRACE_INFO(desc, "Existing file rlink %p %lu " GAH_PRINT_STR,
			       rlink, entry.ino, GAH_PRINT_VAL(out->gah));
		ie_close(desc->fs_handle, desc->ie);
	}
out:
	if (rc)
		IOF_FUSE_REPLY_ERR(request->req, rc);
	else
		fuse_reply_entry(request->req, &entry);
	IOC_REQ_RELEASE(desc);
}

static const struct ioc_request_api api = {
	.get_fsh	= get_fs_handle,
	.on_send	= post_send,
	.on_result	= lookup_cb,
	.on_evict	= ioc_simple_resend
};

#define STAT_KEY lookup

void
ioc_ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct iof_projection_info	*fs_handle = fuse_req_userdata(req);
	struct TYPE_NAME		*desc = NULL;
	struct iof_gah_string_in	*in;
	int rc;

	IOF_TRACE_INFO(req, "Parent:%lu '%s'", parent, name);
	IOC_REQ_INIT_LL(desc, fs_handle, api, in, req, rc);
	if (rc)
		D_GOTO(err, rc);
	IOF_TRACE_INFO(desc, "Req %p ie %p", req, &desc->ie->list);
	in->path = (d_string_t)name;

	/* Find the GAH of the parent */
	rc = find_gah(fs_handle, parent, &in->gah);
	if (rc != 0)
		D_GOTO(err, rc = ENOENT);
	IOC_REQ_SEND_LL(desc, fs_handle, rc);
	if (rc != 0)
		D_GOTO(err, rc);
	return;
err:
	IOC_REQ_RELEASE(desc);
	IOF_FUSE_REPLY_ERR(req, rc);
}
