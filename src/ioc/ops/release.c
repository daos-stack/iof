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
#include "ioc.h"
#include "log.h"
#include "ios_gah.h"

struct release_cb_r {
	struct iof_tracker tracker;
	int err;
};

static void
release_cb(const struct crt_cb_info *cb_info)
{
	struct release_cb_r *reply = cb_info->cci_arg;

	if (cb_info->cci_rc != 0) {
		/*
		 * Error handling.  Return EIO on any error
		 */
		IOF_LOG_INFO("Bad RPC reply %d", cb_info->cci_rc);
		reply->err = EIO;
		iof_tracker_signal(&reply->tracker);
		return;
	}

	iof_tracker_signal(&reply->tracker);
}

int ioc_release(const char *file, struct fuse_file_info *fi)
{
	struct iof_file_handle *handle = (struct iof_file_handle *)fi->fh;
	struct iof_projection_info *fs_handle = handle->fs_handle;
	struct iof_gah_in *in;
	struct release_cb_r reply = {0};
	int rc;

	STAT_ADD(fs_handle->stats, release);

	/* If the projection is off-line then drop the local handle.
	 *
	 * This means a resource leak on the IONSS should the projection
	 * be offline for reasons other than IONSS failure.
	 */
	if (FS_IS_OFFLINE(fs_handle)) {
		iof_pool_release(fs_handle->fh_pool, handle);
		return -fs_handle->offline_reason;
	}

	IOF_LOG_INFO("release %s" GAH_PRINT_STR, file,
		GAH_PRINT_VAL(handle->common.gah));

	if (!handle->common.gah_valid) {
		IOF_LOG_INFO("Release with bad handle %p",
			     handle);

		/* If the server has reported that the GAH is invalid
		 * then do not send a RPC to close it
		 */
		iof_pool_release(fs_handle->fh_pool, handle);
		return -EIO;
	}

	iof_tracker_init(&reply.tracker, 1);
	in = crt_req_get(handle->release_rpc);
	in->gah = handle->common.gah;

	rc = crt_req_send(handle->release_rpc, release_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send rpc, rc = %u", rc);
		iof_pool_release(fs_handle->fh_pool, handle);
		return -EIO;
	}
	crt_req_addref(handle->release_rpc);

	iof_fs_wait(&fs_handle->proj, &reply.tracker);

	iof_pool_release(fs_handle->fh_pool, handle);
	return -reply.err;
}
