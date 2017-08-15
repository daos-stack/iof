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

#include <pouch/list.h>
#include "cnss_plugin.h"
#include "ios_gah.h"
#include "iof_atomic.h"
#include "iof_fs.h"
#include "iof_pool.h"

struct iof_stats {
	ATOMIC unsigned int opendir;
	ATOMIC unsigned int readdir;
	ATOMIC unsigned int closedir;
	ATOMIC unsigned int getattr;
	ATOMIC unsigned int chmod;
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
	ATOMIC unsigned int truncate;
	ATOMIC unsigned int utimens;
	ATOMIC unsigned int read;
	ATOMIC unsigned int write;
	ATOMIC uint64_t read_bytes;
	ATOMIC uint64_t write_bytes;
	ATOMIC unsigned int ftruncate;
	ATOMIC unsigned int getfattr;
	ATOMIC unsigned int fchmod;
	ATOMIC unsigned int futimens;
	ATOMIC unsigned int il_ioctl;
	ATOMIC unsigned int fsync;
};

/*For IOF Plugin*/
struct iof_state {
	struct cnss_plugin_cb		*cb;
	size_t				cb_size;
	/* cart context */
	crt_context_t			crt_ctx;
	crt_list_t			fs_list;
	/* CNSS Prefix */
	char				*cnss_prefix;
	struct ctrl_dir			*ionss_dir;
	struct ctrl_dir			*projections_dir;
	struct iof_group_info		*groups;
	uint32_t			num_groups;

	int progress_thread;

	pthread_t			thread;
	struct iof_tracker		thread_start_tracker;
	struct iof_tracker		thread_stop_tracker;
	struct iof_tracker		thread_shutdown_tracker;
};

struct iof_group_info {
	struct iof_service_group	grp;
	struct ctrl_dir			*group_dir;
	char				*grp_name;
};

struct iof_rb {
	struct fuse_bufvec	buf;
	crt_list_t		list;
};

struct iof_projection_info {
	struct iof_projection		proj;
	struct iof_state		*iof_state;
	struct ios_gah			gah;
	crt_list_t			link;
	/* destination endpoint */
	crt_endpoint_t			dest_ep;
	struct ctrl_dir			*fs_dir;
	struct ctrl_dir			*stats_dir;
	struct iof_stats		*stats;
	char				*mount_point;
	char				*base_dir;
	/* fuse client implementation */
	struct fuse_operations		*fuse_ops;
	/* Feature Flags */
	uint8_t				flags;
	int				fs_id;
	struct iof_pool			pool;
	struct iof_pool_type		*dh;
	struct iof_pool_type		*gh_pool;
	struct iof_pool_type		*fgh_pool;
	struct iof_pool_type		*fh_pool;
	struct iof_pool_type		*rb_pool_small;
	struct iof_pool_type		*rb_pool_page;
	struct iof_pool_type		*rb_pool_large;
	uint32_t			max_read;
	uint32_t			max_write;
	uint32_t			max_iov_read;
	uint32_t			readdir_size;
	/* If set to True then projection is off-line */
	int				offline_reason;
};

#define FS_IS_OFFLINE(HANDLE) ((HANDLE)->offline_reason != 0)

int iof_is_mode_supported(uint8_t flags);

struct fuse_operations *iof_get_fuse_ops(uint8_t flags);

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

/* Data which is stored against an open directory handle */
struct iof_dir_handle {
	struct iof_projection_info	*fs_handle;
	/* The handle for accessing the directory on the IONSS */
	struct ios_gah			gah;
	/* Any RPC reference held across readdir() calls */
	crt_rpc_t			*rpc;
	crt_rpc_t			*open_rpc;
	crt_rpc_t			*close_rpc;
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
	crt_list_t			list;
	/* The name of the directory */
	char				*name;
};

/* Data which is stored against an open file handle */
struct iof_file_handle {
	struct iof_projection_info	*fs_handle;
	struct iof_file_common		common;
	crt_rpc_t			*open_rpc;
	crt_rpc_t			*creat_rpc;
	crt_rpc_t			*release_rpc;
	crt_list_t			list;
	ino_t				inode_no;
	char				*name;
};

struct iof_projection_info *ioc_get_handle(void);

struct status_cb_r {
	struct iof_tracker tracker; /** Completion event tracker */
	int err; /** errno of any internal error */
	int rc;  /** errno reported by remote POSIX operation */
};

struct getattr_cb_r {
	struct iof_tracker tracker;
	int err;
	int rc;
	struct stat *stat;
};

struct getattr_req {
	struct getattr_cb_r		reply;
	crt_endpoint_t			ep;
	struct iof_projection_info	*fs_handle;
	struct crt_rpc			*rpc;
	crt_list_t			list;
};

/* Extract a errno from status_cb_r suitable for returning to FUSE.
 * If err is non-zero then use that, otherwise use rc.  Return negative numbers
 * because IOF uses positive errnos everywhere but FUSE expects negative values.
 *
 * This macro could also with with other *cb_r structs which use the same
 * conventions for err/rc
 */
#define IOC_STATUS_TO_RC(STATUS) \
	((STATUS)->err == 0 ? -(STATUS)->rc : -(STATUS)->err)

/* Check if a remote host is down.  Used in RPC callback to check the cb_info
 * for permanent failure of the remote ep.
 */
#define IOC_HOST_IS_DOWN(CB_INFO) (((CB_INFO)->cci_rc == -CER_TIMEDOUT) || \
					((CB_INFO)->cci_rc == -CER_OOG))

void ioc_mark_ep_offline(struct iof_projection_info *, crt_endpoint_t *);

void ioc_status_cb(const struct crt_cb_info *);

void getattr_cb(const struct crt_cb_info *);

int ioc_opendir(const char *, struct fuse_file_info *);

int ioc_closedir(const char *, struct fuse_file_info *);

int ioc_open(const char *, struct fuse_file_info *);

struct open_cb_r {
	struct iof_file_handle *fh;
	struct iof_tracker tracker;
	int err;
	int rc;
};

void ioc_open_cb(const struct crt_cb_info *);

int ioc_release(const char *, struct fuse_file_info *);

int ioc_create(const char *, mode_t, struct fuse_file_info *);

int ioc_chmod_name(const char *, mode_t);

int ioc_getattr_name(const char *, struct stat *);

int ioc_truncate_name(const char *, off_t);

int ioc_utimens_name(const char *, const struct timespec tv[2]);

int ioc_readdir(const char *, void *, fuse_fill_dir_t, off_t,
		struct fuse_file_info *, enum fuse_readdir_flags);

int ioc_utimens(const char *, const struct timespec tv[2],
		struct fuse_file_info *fi);

int ioc_getattr(const char *, struct stat *, struct fuse_file_info *);

int ioc_truncate(const char *, off_t, struct fuse_file_info *);

int ioc_rename(const char *, const char *, unsigned int);

int ioc_chmod(const char *, mode_t, struct fuse_file_info *);

int ioc_symlink(const char *, const char *);

int ioc_fsync(const char *, int, struct fuse_file_info *);

int ioc_read_buf(const char *, struct fuse_bufvec **, size_t, off_t,
		 struct fuse_file_info *);

int ioc_mkdir(const char *, mode_t);

int ioc_write(const char *, const char *, size_t, off_t,
	      struct fuse_file_info *);

int ioc_rmdir(const char *);

int ioc_unlink(const char *);

int ioc_readlink(const char *, char *, size_t);

int ioc_statfs(const char *, struct statvfs *);

int ioc_ioctl(const char *, int, void *, struct fuse_file_info *,
	      unsigned int, void *);

#endif
