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

	/* O_LARGEFILE should always be set on 64 bit systems, and in fact is
	 * defined to 0 so check that LARGEFILE is set and reject the open
	 * if not.
	 */
	if (!(fi->flags & LARGEFILE)) {
		IOF_LOG_INFO("%p O_LARGEFILE required 0%o", fs_handle,
			     fi->flags);
		return -ENOTSUP;
	}

	/* Check that a regular file is requested */
	if (!(mode & S_IFREG)) {
		IOF_LOG_INFO("%p S_IFREG required 0%o", fs_handle,
			     fi->flags);
		return -ENOTSUP;
	}

	/* Check for flags that do not make sense in this context.
	 */
	if (fi->flags & IOF_UNSUPPORTED_CREATE_FLAGS) {
		IOF_LOG_INFO("%p unsupported flag requested 0%o", fs_handle,
			     fi->flags);
		return -ENOTSUP;
	}

	/* Check that only the flag for a regular file is specified */
	if ((mode & S_IFMT) != S_IFREG) {
		IOF_LOG_INFO("%p unsupported mode requested 0%o", fs_handle,
			     mode);
		return -ENOTSUP;
	}

	if (!IOF_IS_WRITEABLE(fs_handle->flags)) {
		IOF_LOG_INFO("Attempt to modify Read-Only File System");
		return -EROFS;
	}

	handle = iof_pool_acquire(fs_handle->fh_pool);
	if (!handle)
		return -ENOMEM;

	handle->common.projection = &fs_handle->proj;

	IOF_LOG_INFO("file %s flags 0%o mode 0%o handle %p", file, fi->flags,
		     mode, handle);

	iof_tracker_init(&reply.tracker, 1);
	in = crt_req_get(handle->creat_rpc);
	in->path = (crt_string_t)file;
	in->mode = mode;
	in->fs_id = fs_handle->fs_id;
	in->flags = fi->flags;

	reply.fh = handle;

	rc = crt_req_send(handle->creat_rpc, ioc_open_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send rpc, rc = %u", rc);
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
		IOF_LOG_INFO("Handle %p " GAH_PRINT_STR, handle,
			     GAH_PRINT_VAL(handle->common.gah));

	rc = reply.err == 0 ? -reply.rc : -EIO;

	IOF_LOG_DEBUG("path %s handle %p rc %d",
		      handle->name, rc == 0 ? handle : NULL, rc);

	if (rc == 0) {
		fi->fh = (uint64_t)handle;
		handle->common.gah_valid = 1;
	} else {
		iof_pool_release(fs_handle->fh_pool, handle);
	}

	return rc;
}
