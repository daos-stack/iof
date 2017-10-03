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

#include "iof_common.h"
#include "ioc.h"
#include "log.h"
#include "iof_ioctl.h"

int ioc_ioctl(const char *file, int cmd, void *arg, struct fuse_file_info *fi,
	      unsigned int flags, void *data)
{
	struct iof_file_handle *handle = (struct iof_file_handle *)fi->fh;
	struct iof_gah_info gah_info = {0};

	IOF_TRACE_INFO(handle, "ioctl cmd=%#x " GAH_PRINT_STR, cmd,
		       GAH_PRINT_VAL(handle->common.gah));

	STAT_ADD(handle->fs_handle->stats, ioctl);

	if (FS_IS_OFFLINE(handle->fs_handle))
		return -handle->fs_handle->offline_reason;

	if (!handle->common.gah_valid) {
		/* If the server has reported that the GAH, nothing to do */
		return -EIO;
	}

	if (cmd == IOF_IOCTL_GAH) {
		if (data == NULL)
			return -EIO;

		STAT_ADD(handle->fs_handle->stats, il_ioctl);

		/* IOF_IOCTL_GAH has size of gah embedded.  FUSE should have
		 * allocated that many bytes in data
		 */
		IOF_TRACE_INFO(handle, "Requested " GAH_PRINT_STR " fs_id=%d,"
			       " cli_fs_id=%d",
			       GAH_PRINT_VAL(handle->common.gah),
			       handle->fs_handle->fs_id,
			       handle->fs_handle->proj.cli_fs_id);
		gah_info.version = IOF_IOCTL_VERSION;
		gah_info.gah = handle->common.gah;
		gah_info.cnss_id = getpid();
		gah_info.cli_fs_id = handle->fs_handle->proj.cli_fs_id;
		memcpy(data, &gah_info, sizeof(gah_info));
		return 0;
	}

	IOF_TRACE_INFO(handle, "Real ioctl support is not implemented");

	return -ENOTSUP;
}
