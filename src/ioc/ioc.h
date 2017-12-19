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

#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>

#include <gurt/list.h>
#include <gurt/hash.h>

#include "cnss_plugin.h"
#include "ios_gah.h"
#include "iof_atomic.h"
#include "iof_fs.h"
#include "iof_bulk.h"
#include "iof_pool.h"

struct iof_stats {
	ATOMIC unsigned int opendir;
	ATOMIC unsigned int readdir;
	ATOMIC unsigned int closedir;
	ATOMIC unsigned int getattr;
	ATOMIC unsigned int create;
	ATOMIC unsigned int readlink;
	ATOMIC unsigned int rmdir;
	ATOMIC unsigned int mkdir;
	ATOMIC unsigned int statfs;
	ATOMIC unsigned int unlink;
	ATOMIC unsigned int ioctl;
	ATOMIC unsigned int open;
	ATOMIC unsigned int release;
	ATOMIC unsigned int symlink;
	ATOMIC unsigned int rename;
	ATOMIC unsigned int read;
	ATOMIC unsigned int write;
	ATOMIC uint64_t read_bytes;
	ATOMIC uint64_t write_bytes;
	ATOMIC unsigned int il_ioctl;
	ATOMIC unsigned int fsync;
	ATOMIC unsigned int lookup;
	ATOMIC unsigned int forget;
	ATOMIC unsigned int setattr;
};

/* A common structure for holding a cart context and thread details */
struct iof_ctx {
	/* cart context */
	crt_context_t			crt_ctx;
	pthread_t			thread;
	struct iof_pool			*pool;
	struct iof_tracker		thread_start_tracker;
	struct iof_tracker		thread_stop_tracker;
	struct iof_tracker		thread_shutdown_tracker;
	uint32_t			poll_interval;
	crt_progress_cond_cb_t		callback_fn;
};

/*For IOF Plugin*/
struct iof_state {
	struct cnss_plugin_cb		*cb;
	size_t				cb_size;
	struct iof_ctx			iof_ctx;
	d_list_t			fs_list;
	/* CNSS Prefix */
	char				*cnss_prefix;
	struct ctrl_dir			*ionss_dir;
	struct ctrl_dir			*projections_dir;
	struct iof_group_info		*groups;
	uint32_t			num_groups;

};

struct iof_group_info {
	struct iof_service_group	grp;
	char				*grp_name;

	/* Set to true if the CaRT group attached */
	bool				crt_attached;

	/* Set to true if registered with the IONSS */
	bool				iof_registered;
};

struct iof_rb {
	d_list_t			list;
	struct iof_projection_info	*fs_handle;
	struct iof_file_handle		*handle;
	struct fuse_bufvec		fbuf;
	struct iof_local_bulk		lb;
	crt_rpc_t			*rpc;
	fuse_req_t			req;
	struct iof_pool_type		*pt;
	size_t				buf_size;
	bool				failure;
};

struct iof_wb {
	d_list_t			list;
	struct iof_projection_info	*fs_handle;
	struct iof_file_handle		*handle;
	struct iof_local_bulk		lb;
	crt_rpc_t			*rpc;
	fuse_req_t			req;
	bool				failure;
};

struct iof_projection_info {
	struct iof_projection		proj;
	struct iof_ctx			ctx;
	struct iof_state		*iof_state;
	struct ios_gah			gah;
	d_list_t			link;
	struct ctrl_dir			*fs_dir;
	struct ctrl_dir			*stats_dir;
	struct iof_stats		*stats;
	/* The name of the mount directory */
	struct ios_name			mnt_dir;
	char				*mount_point;

	/* The name of the ctrlfs direcory */
	struct ios_name			ctrl_dir;
	/* fuse client implementation */
	struct fuse_lowlevel_ops	*fuse_ops;
	/* Feature Flags */
	uint64_t			flags;
	int				fs_id;
	struct iof_pool			pool;
	struct iof_pool_type		*dh_pool;
	struct iof_pool_type		*fgh_pool;
	struct iof_pool_type		*close_pool;
	struct iof_pool_type		*lookup_pool;
	struct iof_pool_type		*mkdir_pool;
	struct iof_pool_type		*symlink_pool;
	struct iof_pool_type		*fh_pool;
	struct iof_pool_type		*rb_pool_page;
	struct iof_pool_type		*rb_pool_large;
	struct iof_pool_type		*write_pool;
	uint32_t			max_read;
	uint32_t			max_iov_read;
	uint32_t			readdir_size;
	/* set to error code if projection is off-line */
	int				offline_reason;
	struct d_chash_table		inode_ht;

	/* List of directory handles owned by FUSE */
	pthread_mutex_t			od_lock;
	d_list_t			opendir_list;
};

#define FS_IS_OFFLINE(HANDLE) ((HANDLE)->offline_reason != 0)

int iof_is_mode_supported(uint8_t flags);

struct fuse_lowlevel_ops *iof_get_fuse_ops(uint64_t);

/* Everything above here relates to how the ION plugin communicates with the
 * CNSS, everything below here relates to internals to the plugin.  At some
 * point we should split this header file up into two.
 */

#define STAT_ADD(STATS, STAT) atomic_inc(&STATS->STAT)
#define STAT_ADD_COUNT(STATS, STAT, COUNT) atomic_add(&STATS->STAT, COUNT)

/* Helper macros for open() and creat() to log file access modes */
#define LOG_MODE(HANDLE, FLAGS, MODE) do {			\
		if ((FLAGS) & (MODE))				\
			IOF_LOG_DEBUG("%p " #MODE, HANDLE);	\
		FLAGS &= ~MODE;					\
	} while (0)

/* Dump the file open mode to the logile
 *
 * On a 64 bit system O_LARGEFILE is assumed so always set but defined to zero
 * so set LARGEFILE here for debugging
 */
#define LARGEFILE 0100000
#define LOG_FLAGS(HANDLE, INPUT) do {					\
		int _flag = (INPUT);					\
		LOG_MODE((HANDLE), _flag, O_APPEND);			\
		LOG_MODE((HANDLE), _flag, O_RDONLY);			\
		LOG_MODE((HANDLE), _flag, O_WRONLY);			\
		LOG_MODE((HANDLE), _flag, O_RDWR);			\
		LOG_MODE((HANDLE), _flag, O_ASYNC);			\
		LOG_MODE((HANDLE), _flag, O_CLOEXEC);			\
		LOG_MODE((HANDLE), _flag, O_CREAT);			\
		LOG_MODE((HANDLE), _flag, O_DIRECT);			\
		LOG_MODE((HANDLE), _flag, O_DIRECTORY);			\
		LOG_MODE((HANDLE), _flag, O_DSYNC);			\
		LOG_MODE((HANDLE), _flag, O_EXCL);			\
		LOG_MODE((HANDLE), _flag, O_LARGEFILE);			\
		LOG_MODE((HANDLE), _flag, LARGEFILE);			\
		LOG_MODE((HANDLE), _flag, O_NOATIME);			\
		LOG_MODE((HANDLE), _flag, O_NOCTTY);			\
		LOG_MODE((HANDLE), _flag, O_NONBLOCK);			\
		LOG_MODE((HANDLE), _flag, O_PATH);			\
		LOG_MODE((HANDLE), _flag, O_SYNC);			\
		LOG_MODE((HANDLE), _flag, O_TRUNC);			\
		if (_flag)						\
			IOF_LOG_ERROR("%p Flags 0%o", (HANDLE), _flag);	\
	} while (0)

/* Dump the file mode to the logfile
 */
#define LOG_MODES(HANDLE, INPUT) do {					\
		int _flag = (INPUT) & S_IFMT;				\
		LOG_MODE((HANDLE), _flag, S_IFREG);			\
		LOG_MODE((HANDLE), _flag, S_ISUID);			\
		LOG_MODE((HANDLE), _flag, S_ISGID);			\
		LOG_MODE((HANDLE), _flag, S_ISVTX);			\
		if (_flag)						\
			IOF_LOG_ERROR("%p Mode 0%o", (HANDLE), _flag);	\
	} while (0)

#define IOF_UNSUPPORTED_CREATE_FLAGS (O_ASYNC | O_CLOEXEC | O_DIRECTORY | \
					O_NOCTTY | O_PATH)

#define IOF_UNSUPPORTED_OPEN_FLAGS (IOF_UNSUPPORTED_CREATE_FLAGS | O_CREAT | \
					O_EXCL)

#define IOF_FUSE_REPLY_ERR(req, status)					\
	do {								\
		int __err = status;					\
		int __rc;						\
		if (__err <= 0) {					\
			IOF_TRACE_ERROR(req, "Invalid value passed to reply_err: %d", \
					__err);				\
			__err = EIO;					\
		}							\
		if (__err == ENOTSUP || __err == EIO)			\
			IOF_TRACE_WARNING(req, "Returning %d '%s'",	\
					  __err, strerror(__err));	\
		else							\
			IOF_TRACE_DEBUG(req, "Returning %d '%s'",	\
					__err, strerror(__err));	\
		__rc = fuse_reply_err(req, __err);			\
		if (__rc != 0)						\
			IOF_TRACE_ERROR(req, "fuse_reply_err returned %d:%s", \
					__rc, strerror(-__rc));		\
		IOF_TRACE_DOWN(req);					\
	} while (0)

#define IOF_FUSE_REPLY_ZERO(req)					\
	do {								\
		int __rc;						\
		IOF_TRACE_DEBUG(req, "Returning 0");			\
		__rc = fuse_reply_err(req, 0);				\
		if (__rc != 0)						\
			IOF_TRACE_ERROR(req, "fuse_reply_err returned %d:%s", \
					__rc, strerror(-__rc));		\
		IOF_TRACE_DOWN(req);					\
	} while (0)

#define IOF_FUSE_REPLY_ATTR(req, attr)					\
	do {								\
		int __rc;						\
		IOF_TRACE_DEBUG(req, "Returning attr");			\
		__rc = fuse_reply_attr(req, attr, 0);			\
		if (__rc != 0)						\
			IOF_TRACE_ERROR(req, "fuse_reply_attr returned %d:%s", \
					__rc, strerror(-__rc));		\
		IOF_TRACE_DOWN(req);					\
	} while (0)

#define IOF_FUSE_REPLY_WRITE(req, bytes)				\
	do {								\
		int __rc;						\
		IOF_TRACE_DEBUG(req, "Returning write(%zi)", bytes);	\
		__rc = fuse_reply_write(req, bytes);			\
		if (__rc != 0)						\
			IOF_TRACE_ERROR(req, "fuse_reply_attr returned %d:%s", \
					__rc, strerror(-__rc));		\
		IOF_TRACE_DOWN(req);					\
	} while (0)

struct ioc_request;

struct ioc_request_api {
	struct iof_projection_info	*(*get_fsh)(struct ioc_request *req);
	void				(*on_send)(struct ioc_request *req);
	void				(*on_result)(struct ioc_request *req);
	int				(*on_evict)(struct ioc_request *req);
};

struct ioc_request {
	int				rc;
	int				err;
	crt_endpoint_t			ep;
	crt_rpc_t			*rpc;
	fuse_req_t			req;
	const struct ioc_request_api	*cb;
};

/* Data which is stored against an open directory handle */
struct iof_dir_handle {
	struct ioc_request		open_req;
	struct ioc_request		close_req;
	struct iof_projection_info	*fs_handle;
	/* The handle for accessing the directory on the IONSS */
	struct ios_gah			gah;
	/* Any RPC reference held across readdir() calls */
	crt_rpc_t			*rpc;
	/* Pointer to any retreived data from readdir() RPCs */
	struct iof_readdir_reply	*replies;
	int				reply_count;
	void				*replies_base;
	/* Set to True if the current batch of replies is the final one */
	int				last_replies;
	/* Set to 1 initially, but 0 if there is a unrecoverable error */
	int				handle_valid;
	/* Set to 0 if the server rejects the GAH at any point */
	int				gah_valid;
	crt_endpoint_t			ep;
	d_list_t			list;
};

/* Data which is stored against an open file handle */
struct iof_file_handle {
	struct iof_projection_info	*fs_handle;
	struct iof_file_common		common;
	crt_rpc_t			*open_rpc;
	crt_rpc_t			*creat_rpc;
	crt_rpc_t			*release_rpc;
	d_list_t			list;
	ino_t				inode_no;
	char				*name;

	struct ioc_inode_entry		*ie;

	/* Fuse req for open/create command */
	fuse_req_t			open_req;
};

struct iof_projection_info *ioc_get_handle(void);

struct status_cb_r {
	int err; /** errno of any internal error */
	int rc;  /** errno reported by remote POSIX operation */
	struct iof_tracker tracker; /** Completion event tracker */
};

struct getattr_req {
	struct ioc_request		request;
	struct iof_projection_info	*fs_handle;
	d_list_t			list;
};

struct close_req {
	struct ioc_request		request;
	struct iof_projection_info	*fs_handle;
	d_list_t			list;
};

struct ioc_inode_entry {
	struct ios_gah	gah;
	char		name[256];
	d_list_t	list;
	fuse_ino_t	ino;
	fuse_ino_t	parent;
	ATOMIC uint	ref;
};

struct entry_req {
	struct iof_projection_info	*fs_handle;
	struct ioc_request		request;
	struct ioc_inode_entry		*ie;
	d_list_t			list;
	crt_opcode_t			opcode;
	char				*dest;
};

/* inode.c */

/* Convert from a inode to a GAH using the hash table */
int find_gah(struct iof_projection_info *, fuse_ino_t, struct ios_gah *);

/* Convert from a inode to a GAH and keep a reference using the hash table */
int find_gah_ref(struct iof_projection_info *, fuse_ino_t, struct ios_gah *);

/* Drop a reference on the GAH in the hash table */
void drop_ino_ref(struct iof_projection_info *, fuse_ino_t);

void ie_close(struct iof_projection_info *, struct ioc_inode_entry *);

/* Extract a errno from status_cb_r suitable for returning to FUSE.
 * If err is non-zero then use that, otherwise use rc.  Return negative numbers
 * because IOF uses positive errnos everywhere but FUSE expects negative values.
 *
 * This macro could also with with other *cb_r structs which use the same
 * conventions for err/rc
 */

#define IOC_STATUS_TO_RC_LL(STATUS) \
	((STATUS)->err == 0 ? (STATUS)->rc : EIO)

#define IOC_GET_RESULT(REQ) crt_reply_get((REQ)->rpc)

/* Correctly resolve the return codes and errors from the RPC response.
 * If the error code was already non-zero, it means an error occurred on
 * the client; do nothing. A non-zero error code in the RPC response
 * denotes a server error, in which case, set the status error code to EIO.
 *
 * TODO: Unify the RPC output types so they can be processed without using
 * this macro, since the 'err' and 'rc' fields are common in all of them.
 */
#define IOC_RESOLVE_STATUS(STATUS, OUT)					\
	do {								\
		if ((OUT) != NULL) {					\
			if (!(STATUS)->err) {				\
				(STATUS)->rc = (OUT)->rc;		\
				if ((OUT)->err)				\
					(STATUS)->err = EIO;		\
			}						\
		}							\
	} while (0)

int iof_fs_send(struct ioc_request *request);

int ioc_simple_resend(struct ioc_request *request);

void ioc_ll_gen_cb(const struct crt_cb_info *);

void ioc_ll_lookup(fuse_req_t, fuse_ino_t, const char *);

void ioc_ll_forget(fuse_req_t, fuse_ino_t, uint64_t);

void ioc_ll_forget_multi(fuse_req_t, size_t, struct fuse_forget_data *);

void ioc_ll_getattr(fuse_req_t, fuse_ino_t, struct fuse_file_info *);

void ioc_ll_statfs(fuse_req_t, fuse_ino_t);

void ioc_ll_readlink(fuse_req_t, fuse_ino_t);

void ioc_ll_mkdir(fuse_req_t, fuse_ino_t, const char *, mode_t);

void ioc_ll_open(fuse_req_t, fuse_ino_t, struct fuse_file_info *);

void ioc_ll_create(fuse_req_t, fuse_ino_t, const char *, mode_t,
		   struct fuse_file_info *);

void ioc_ll_read(fuse_req_t, fuse_ino_t, size_t, off_t,
		 struct fuse_file_info *);

void ioc_ll_release(fuse_req_t, fuse_ino_t, struct fuse_file_info *);

void ioc_ll_unlink(fuse_req_t, fuse_ino_t, const char *);

void ioc_ll_rmdir(fuse_req_t, fuse_ino_t, const char *);

void ioc_ll_opendir(fuse_req_t, fuse_ino_t, struct fuse_file_info *);

void ioc_ll_readdir(fuse_req_t, fuse_ino_t, size_t, off_t,
		    struct fuse_file_info *);

void ioc_ll_rename(fuse_req_t, fuse_ino_t, const char *, fuse_ino_t,
		   const char *, unsigned int);

void ioc_ll_releasedir(fuse_req_t, fuse_ino_t, struct fuse_file_info *);

void ioc_int_releasedir(struct iof_dir_handle *);

void ioc_ll_write(fuse_req_t, fuse_ino_t, const char *,	size_t, off_t,
		  struct fuse_file_info *);

void ioc_ll_write_buf(fuse_req_t, fuse_ino_t, struct fuse_bufvec *,
		      off_t, struct fuse_file_info *);

void ioc_ll_ioctl(fuse_req_t, fuse_ino_t, int, void *, struct fuse_file_info *,
		  unsigned, const void *, size_t, size_t);

void ioc_ll_setattr(fuse_req_t, fuse_ino_t, struct stat *, int,
		    struct fuse_file_info *);

void ioc_ll_symlink(fuse_req_t, const char *, fuse_ino_t, const char *);

void ioc_ll_fsync(fuse_req_t, fuse_ino_t, int, struct fuse_file_info *);

void iof_entry_cb(struct ioc_request *request);

#endif
