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

struct closedir_cb_r {
	int complete;
};

static int closedir_cb(const struct crt_cb_info *cb_info)
{
	struct closedir_cb_r *reply = cb_info->cci_arg;

	/* There is no error handling needed here, as all client state will be
	 * destroyed on return anyway.
	 */

	reply->complete = 1;
	return 0;
}

int ioc_closedir(const char *dir, struct fuse_file_info *fi)
{
	struct iof_dir_handle *dir_handle = (struct iof_dir_handle *)fi->fh;
	struct iof_projection_info *fs_handle = dir_handle->fs_handle;
	struct iof_gah_in *in;
	struct closedir_cb_r reply = {0};
	crt_rpc_t *rpc = NULL;
	int rc;

	STAT_ADD(fs_handle->stats, closedir);

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(dir_handle->gah));

	if (FS_IS_OFFLINE(fs_handle))
		return -fs_handle->offline_reason;

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

	rc = crt_req_create(fs_handle->proj.crt_ctx, fs_handle->dest_ep,
			    FS_TO_OP(fs_handle, closedir), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u",
			      rc);
		rc = EIO;
		goto out;
	}

	in = crt_req_get(rpc);
	in->gah = dir_handle->gah;

	rc = crt_req_send(rpc, closedir_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send rpc, rc = %u", rc);
		rc = EIO;
		goto out;
	}

	rc = iof_fs_progress(&fs_handle->proj, &reply.complete);

out:
	/* If there has been an error on the local handle, or readdir() is not
	 * exhausted then ensure that all resources are freed correctly
	 */
	if (dir_handle->rpc)
		crt_req_decref(dir_handle->rpc);

	free(dir_handle);
	return -rc;
}

