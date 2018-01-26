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

void ioc_release_priv(fuse_req_t req, struct iof_file_handle *handle)
{
	struct iof_projection_info *fs_handle = handle->fs_handle;
	struct iof_gah_in *in;
	int ret = EIO;
	int rc;

	STAT_ADD(fs_handle->stats, release);

	pthread_mutex_lock(&handle->fs_handle->of_lock);
	d_list_del(&handle->list);
	pthread_mutex_unlock(&handle->fs_handle->of_lock);

	/* If the projection is off-line then drop the local handle.
	 *
	 * This means a resource leak on the IONSS should the projection
	 * be offline for reasons other than IONSS failure.
	 */
	if (FS_IS_OFFLINE(fs_handle))
		D_GOTO(out_err, ret = fs_handle->offline_reason);

	IOF_TRACE_UP(req, handle, "release_fuse_req");

	IOF_TRACE_INFO(req, GAH_PRINT_STR,
		       GAH_PRINT_VAL(handle->common.gah));

	if (!handle->common.gah_valid) {
		IOF_TRACE_INFO(req, "Release with bad handle");

		/* If the server has reported that the GAH is invalid
		 * then do not send a RPC to close it.
		 */
		D_GOTO(out_err, ret = EIO);
	}

	in = crt_req_get(handle->release_rpc);
	in->gah = handle->common.gah;
	IOF_TRACE_LINK(handle->release_rpc, req, "release_file_rpc");

	crt_req_addref(handle->release_rpc);
	rc = crt_req_send(handle->release_rpc, ioc_ll_gen_cb, req);
	if (rc) {
		crt_req_decref(handle->release_rpc);
		D_GOTO(out_err, ret = EIO);
	}

	iof_pool_release(fs_handle->fh_pool, handle);
	return;

out_err:
	iof_pool_release(fs_handle->fh_pool, handle);
	if (req)
		IOF_FUSE_REPLY_ERR(req, ret);

}

void ioc_ll_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct iof_file_handle *fh = (struct iof_file_handle *)fi->fh;

	ioc_release_priv(req, fh);
}

void ioc_int_release(struct iof_file_handle *fh)
{
	ioc_release_priv(NULL, fh);
}
