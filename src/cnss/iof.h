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
#ifndef __IOF_H__
#define __IOF_H__

#include "cnss_plugin.h"
#include "ios_gah.h"

int iof_plugin_init(struct cnss_plugin **fns, size_t *size);

/* For IOF Plugin */
struct iof_state {
	struct cnss_plugin_cb *cb;
	size_t cb_size;

	/* destination group */
	crt_group_t	*dest_group;
	crt_endpoint_t	psr_ep;
	/* cart context */
	crt_context_t	crt_ctx;
	struct proto	*proto;
	/* CNSS Prefix */
	char		*cnss_prefix;

};

/* For each projection */
struct fs_handle {
	struct iof_state	*iof_state;
	/* destination endpoint */
	crt_endpoint_t		dest_ep;
	/* cart context */
	crt_context_t		crt_ctx;
	struct proto		*proto;
	int			fs_id;
};

#define FS_TO_OP(HANDLE, FN) ((HANDLE)->proto->mt.FN.op_id)

/* Everything above here relates to how the ION plugin communicates with the
 * CNSS, everything below here relates to internals to the plugin.  At some
 * point we should split this header file up into two.
 */

/* Data which is stored against an open directory handle */
struct iof_dir_handle {
	struct fs_handle	*fs_handle;
	/* The handle for accessing the directory on the IONSS */
	struct ios_gah		gah;
	/* Any RPC reference held across readdir() calls */
	crt_rpc_t		*rpc;
	/* Pointer to any retreived data from readdir() RPCs */
	struct iof_readdir_reply *replies;
	int			reply_count;
	/* Set to 1 initially, but 0 if there is a unrecoverable error */
	int			handle_valid;
	/* Set to 0 if the server rejects the GAH at any point */
	int			gah_valid;
	/* The name of the directory */
	char			name[];
};

/* Data which is stored against an open file handle */
struct iof_file_handle {
	struct fs_handle	*fs_handle;
	struct ios_gah		gah;
	int			gah_valid;
	char			name[];
};

struct fs_handle *ioc_get_handle(void);

struct status_cb_r {
	int complete; /** Flag to check for operation complete */
	int err;      /** errno of any internal error */
	int rc;       /** errno reported by remote POSIX operation */
};

/* Extract a errno from status_cb_r suitable for returning to FUSE.
 * If err is non-zero then use that, otherwise use rc.  Return negative numbers
 * because IOF uses positive errnos everywhere but FUSE expects negative values.
 *
 * This macro could also with with other *cb_r structs which use the same
 * conventions for err/rc
 */
#define IOC_STATUS_TO_RC(STATUS) (STATUS.err == 0 ? -STATUS.rc : -STATUS.err)

int ioc_status_cb(const struct crt_cb_info *);

int ioc_cb_progress(struct fs_handle *, int *);

int ioc_opendir(const char *, struct fuse_file_info *);

int ioc_closedir(const char *, struct fuse_file_info *);

int ioc_open(const char *, struct fuse_file_info *);

struct open_cb_r {
	struct iof_file_handle *fh;
	int complete;
	int err;
	int rc;
};

int ioc_open_cb(const struct crt_cb_info *);

struct iof_file_handle *ioc_fh_new(const char *);

int ioc_release(const char *, struct fuse_file_info *);

int ioc_create(const char *, mode_t, struct fuse_file_info *);

#if IOF_USE_FUSE3
int ioc_readdir(const char *, void *, fuse_fill_dir_t, off_t,
		struct fuse_file_info *, enum fuse_readdir_flags);

int ioc_getattr(const char *, struct stat *, struct fuse_file_info *);

int ioc_truncate(const char *, off_t, struct fuse_file_info *);

int ioc_rename3(const char *, const char *, unsigned int);

int ioc_chmod(const char *, mode_t, struct fuse_file_info *);
#else
int ioc_readdir(const char *, void *, fuse_fill_dir_t, off_t,
		struct fuse_file_info *);

int ioc_getattr_name(const char *, struct stat *);

int ioc_truncate_name(const char *, off_t);

int ioc_rename(const char *, const char *);

int ioc_chmod_name(const char *, mode_t);
#endif

int ioc_symlink(const char *, const char *);

int ioc_fsync(const char *, int, struct fuse_file_info *);

int ioc_read(const char *, char *, size_t, off_t, struct fuse_file_info *);

int ioc_mkdir(const char *, mode_t);

int ioc_write(const char *, const char *, size_t, off_t,
	      struct fuse_file_info *);

int ioc_rmdir(const char *);

int ioc_unlink(const char *);

int ioc_readlink(const char *, char *, size_t);

#endif
