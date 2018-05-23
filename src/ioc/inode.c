/* Copyright (C) 2017-2018 Intel Corporation
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

#define POOL_NAME close_pool
#define TYPE_NAME common_req
#define REQ_NAME request
#define STAT_KEY release
#define RESTOCK_ON_SEND
#include "ioc_ops.h"

/* Find a GAH from a inode, return 0 if found */
static int
find_gah_internal(struct iof_projection_info *fs_handle,
		  ino_t ino, struct ios_gah *gah, bool drop_ref)
{
	struct ioc_inode_entry *ie;
	d_list_t *rlink;

	if (ino == 1) {
		D_MUTEX_LOCK(&fs_handle->gah_lock);
		*gah = fs_handle->gah;
		D_MUTEX_UNLOCK(&fs_handle->gah_lock);
		return 0;
	}

	rlink = d_hash_rec_find(&fs_handle->inode_ht, &ino, sizeof(ino));
	if (!rlink)
		return -1;

	ie = container_of(rlink, struct ioc_inode_entry, list);

	IOF_TRACE_INFO(ie, "Inode %lu " GAH_PRINT_STR, ie->stat.st_ino,
		       GAH_PRINT_VAL(ie->gah));

	D_MUTEX_LOCK(&fs_handle->gah_lock);
	*gah = ie->gah;
	D_MUTEX_UNLOCK(&fs_handle->gah_lock);

	/* Once the GAH has been copied drop the reference on the parent inode
	 */
	if (drop_ref)
		d_hash_rec_decref(&fs_handle->inode_ht, rlink);
	return 0;
}

int
find_gah(struct iof_projection_info *fs_handle,
	 fuse_ino_t ino,
	 struct ios_gah *gah)
{
	return find_gah_internal(fs_handle, ino, gah, true);
}

int
find_gah_ref(struct iof_projection_info *fs_handle,
	     fuse_ino_t ino,
	     struct ios_gah *gah)
{
	return find_gah_internal(fs_handle, ino, gah, false);
}

/* Drop a reference on the GAH in the hash table */
void
drop_ino_ref(struct iof_projection_info *fs_handle, ino_t ino)
{
	d_list_t *rlink;

	if (ino == 1)
		return;

	rlink = d_hash_rec_find(&fs_handle->inode_ht, &ino, sizeof(ino));

	if (!rlink) {
		IOF_TRACE_WARNING(fs_handle, "Could not find entry %lu", ino);
		return;
	}
	d_hash_rec_ndecref(&fs_handle->inode_ht, 2, rlink);
}

static void ie_close_cb(struct ioc_request *request)
{
	struct TYPE_NAME	*desc = CONTAINER(request);

	IOC_REQ_RELEASE_POOL(desc, desc->fs_handle->close_pool);
}

static const struct ioc_request_api api = {
	.on_send	= post_send,
	.on_result	= ie_close_cb,
};

void ie_close(struct iof_projection_info *fs_handle, struct ioc_inode_entry *ie)
{
	struct TYPE_NAME	*desc = NULL;
	struct iof_gah_in	*in;
	int			rc;

	if (FS_IS_OFFLINE(fs_handle))
		D_GOTO(err, rc = fs_handle->offline_reason);

	if (ie->gah.root != atomic_load_consume(&fs_handle->proj.grp->pri_srv_rank)) {
		IOF_TRACE_WARNING(fs_handle,
				  "Gah with old root %lu " GAH_PRINT_STR,
				  ie->stat.st_ino, GAH_PRINT_VAL(ie->gah));
		D_GOTO(out, 0);
		return;
	}

	IOF_TRACE_INFO(ie, GAH_PRINT_STR, GAH_PRINT_VAL(ie->gah));

	IOC_REQ_INIT(desc, fs_handle, api, in, rc);
	if (rc)
		D_GOTO(err, 0);

	D_MUTEX_LOCK(&fs_handle->gah_lock);
	in->gah = ie->gah;
	D_MUTEX_UNLOCK(&fs_handle->gah_lock);

	IOC_REQ_SEND_LL(desc, fs_handle, rc);
	if (rc != 0)
		D_GOTO(err, 0);

	return;

err:
	IOF_TRACE_ERROR(ie, "Failed to close " GAH_PRINT_STR " %d",
			GAH_PRINT_VAL(ie->gah), rc);
out:
	IOC_REQ_RELEASE_POOL(desc, fs_handle->close_pool);
}
