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

void
ioc_ll_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
	     struct fuse_file_info *fi)
{
	struct iof_projection_info	*fs_handle = fuse_req_userdata(req);
	struct iof_file_handle *handle = (struct iof_file_handle *)fi->fh;
	struct iof_gah_in *in;
	crt_rpc_t *rpc = NULL;
	crt_opcode_t opcode;
	int rc;

	STAT_ADD(fs_handle->stats, fsync);

	if (FS_IS_OFFLINE(fs_handle))
		D_GOTO(out, rc = fs_handle->offline_reason);

	if (!IOF_IS_WRITEABLE(fs_handle->flags))
		D_GOTO(out, rc = EROFS);

	IOF_TRACE_INFO(handle);

	if (!H_GAH_IS_VALID(handle))
		/* If the server has reported that the GAH is invalid
		 * then do not try to do anything with it.
		 */
		D_GOTO(out, rc = EIO);

	if (datasync)
		opcode = FS_TO_OP(fs_handle, fdatasync);
	else
		opcode = FS_TO_OP(fs_handle, fsync);

	rc = crt_req_create(fs_handle->proj.crt_ctx, &handle->common.ep, opcode,
			    &rpc);
	if (rc || !rpc)
		D_GOTO(out, rc = EIO);

	in = crt_req_get(rpc);
	pthread_mutex_lock(&fs_handle->gah_lock);
	in->gah = handle->common.gah;
	pthread_mutex_unlock(&fs_handle->gah_lock);

	rc = crt_req_send(rpc, ioc_ll_gen_cb, req);
	if (rc)
		D_GOTO(out, rc = EIO);

	return;
out:

	IOF_FUSE_REPLY_ERR(req, rc);
}
