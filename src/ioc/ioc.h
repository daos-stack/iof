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

/**
 * \file
 *
 * CNSS/IOF client headers.
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

/**
 * A common structure for holding a cart context and thread details.
 *
 * This is included in both iof_state for global values, and once per
 * projection for projection specific entries.
 */
struct iof_ctx {
	/** cart context */
	crt_context_t			crt_ctx;
	/** pthread identifier */
	pthread_t			thread;
	/** Object pool.  This is used on projections to flush on shutdown */
	struct iof_pool			*pool;
	/** Tracker to detect thread start */
	struct iof_tracker		thread_start_tracker;
	/** Tracker to signal thread stop */
	struct iof_tracker		thread_stop_tracker;
	/** Tracker to detect thread stop */
	struct iof_tracker		thread_shutdown_tracker;
	/** Poll interval to pass to crt_progress */
	uint32_t			poll_interval;
	/** Callback function to pass to crt_progress() */
	crt_progress_cond_cb_t		callback_fn;
};

/**
 * Global state for IOF client.
 *
 */
struct iof_state {
	/** Callback to CNSS plugin */
	struct cnss_plugin_cb		*cb;
	/** CaRT RPC protocol used */
	struct crt_proto_format		*proto;
	/** iof_ctx for state */
	struct iof_ctx			iof_ctx;
	/** List of projections */
	d_list_t			fs_list;
	/** CNSS Prefix.  Parent directory of projections */
	char				*cnss_prefix;
	/** ctrl_fs inoss directory handle */
	struct ctrl_dir			*ionss_dir;
	/** ctrl_fs projections directory handle */
	struct ctrl_dir			*projections_dir;
	/** Group information */
	struct iof_group_info		*groups;
	/** Number of groups, will be 1 */
	uint32_t			num_groups;
	/** Number of projections */
	uint32_t			num_proj;
};

/**
 * IOF Group struct.
 *
 * Intended to be used to support multiple groups but support for that is not
 * in place yet so only 1 group is currently allowed.
 *
 */

struct iof_group_info {
	/** Service group pointer */
	struct iof_service_group	grp;
	/** The group name */
	char				*grp_name;

	/** Set to true if the CaRT group attached */
	bool				crt_attached;

	/** Set to true if registered with the IONSS */
	bool				iof_registered;
};

/** Read buffer descriptor */
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

/** Write buffer descriptor */
struct iof_wb {
	d_list_t			list;
	struct iof_projection_info	*fs_handle;
	struct iof_file_handle		*handle;
	struct iof_local_bulk		lb;
	crt_rpc_t			*rpc;
	fuse_req_t			req;
	bool				failure;
};

enum iof_failover_state {
	iof_failover_running,
	iof_failover_offline,
	iof_failover_in_progress,
	iof_failover_complete,
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
	struct fuse_session		*session;
	/** The basename of the mount point */
	struct ios_name			mnt_dir;
	/** The mount location */
	char				*mount_point;

	enum iof_failover_state		failover_state;

	/** The name of the ctrlfs directory */
	struct ios_name			ctrl_dir;
	/** fuse client implementation */
	struct fuse_lowlevel_ops	*fuse_ops;
	/** Feature Flags */
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
	/** set to error code if projection is off-line */
	int				offline_reason;
	/** Hash table of open inodes */
	struct d_hash_table		inode_ht;

	pthread_mutex_t			od_lock;
	/** List of directory handles owned by FUSE */
	d_list_t			opendir_list;

	pthread_mutex_t			of_lock;
	/** List of open file handles owned by FUSE */
	d_list_t			openfile_list;

	/** Held for any access/modification to a gah on any inode/file/dir */
	pthread_mutex_t			gah_lock;

	/** Reference count for pending migrate RPCS */
	ATOMIC int			p_gah_update_count;

	/** List of requests to be actioned when failover completes */
	d_list_t			p_requests_pending;
	pthread_mutex_t			p_request_lock;

	/** List of child inodes.
	 *
	 * Populated during failover only, should be empty if not a
	 * directory.
	 */
	d_list_t			p_ie_children;
};

#define FS_IS_OFFLINE(HANDLE) ((HANDLE)->offline_reason != 0)

/*
 * Returns the correct RPC Type ID from the protocol registry.
 */
#define FS_TO_OP(HANDLE, FN) (CRT_PROTO_OPC((HANDLE)->proj.proto->cpf_base, \
					    (HANDLE)->proj.proto->cpf_ver, \
					    DEF_RPC_TYPE(FN)))

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

/**
 * Dump the file open mode to the logile.
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

/** Dump the file mode to the logfile. */
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
			IOF_TRACE_ERROR(req,				\
					"Invalid call to fuse_reply_err: %d", \
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
			IOF_TRACE_ERROR(req,				\
					"fuse_reply_err returned %d:%s", \
					__rc, strerror(-__rc));		\
		IOF_TRACE_DOWN(req);					\
	} while (0)

#define IOF_FUSE_REPLY_ZERO(req)					\
	do {								\
		int __rc;						\
		IOF_TRACE_DEBUG(req, "Returning 0");			\
		__rc = fuse_reply_err(req, 0);				\
		if (__rc != 0)						\
			IOF_TRACE_ERROR(req,				\
					"fuse_reply_err returned %d:%s", \
					__rc, strerror(-__rc));		\
		IOF_TRACE_DOWN(req);					\
	} while (0)

#define IOF_FUSE_REPLY_ATTR(req, attr)					\
	do {								\
		int __rc;						\
		IOF_TRACE_DEBUG(req, "Returning attr");			\
		__rc = fuse_reply_attr(req, attr, 0);			\
		if (__rc != 0)						\
			IOF_TRACE_ERROR(req,				\
					"fuse_reply_attr returned %d:%s", \
					__rc, strerror(-__rc));		\
		IOF_TRACE_DOWN(req);					\
	} while (0)

#define IOF_FUSE_REPLY_WRITE(req, bytes)				\
	do {								\
		int __rc;						\
		IOF_TRACE_DEBUG(req, "Returning write(%zi)", bytes);	\
		__rc = fuse_reply_write(req, bytes);			\
		if (__rc != 0)						\
			IOF_TRACE_ERROR(req,				\
					"fuse_reply_attr returned %d:%s", \
					__rc, strerror(-__rc));		\
		IOF_TRACE_DOWN(req);					\
	} while (0)

#define IOF_FUSE_REPLY_OPEN(req, fi)					\
	do {								\
		int __rc;						\
		IOF_TRACE_DEBUG(req, "Returning open");			\
		__rc = fuse_reply_open(req, &fi);			\
		if (__rc != 0)						\
			IOF_TRACE_ERROR(req,				\
					"fuse_reply_open returned %d:%s", \
					__rc, strerror(-__rc));		\
		IOF_TRACE_DOWN(req);					\
	} while (0)

#define IOF_FUSE_REPLY_CREATE(req, entry, fi)				\
	do {								\
		int __rc;						\
		IOF_TRACE_DEBUG(req, "Returning create");		\
		__rc = fuse_reply_create(req, &entry, &fi);		\
		if (__rc != 0)						\
			IOF_TRACE_ERROR(req,				\
					"fuse_reply_create returned %d:%s",\
					__rc, strerror(-__rc));		\
		IOF_TRACE_DOWN(req);					\
	} while (0)

#define IOF_FUSE_REPLY_ENTRY(req, entry)				\
	do {								\
		int __rc;						\
		IOF_TRACE_DEBUG(req, "Returning entry");		\
		__rc = fuse_reply_entry(req, &entry);			\
		if (__rc != 0)						\
			IOF_TRACE_ERROR(req,				\
					"fuse_reply_entry returned %d:%s", \
					__rc, strerror(-__rc));		\
		IOF_TRACE_DOWN(req);					\
	} while (0)

#define IOF_FUSE_REPLY_STATFS(req, stat)				\
	do {								\
		int __rc;						\
		IOF_TRACE_DEBUG(req, "Returning statfs");		\
		__rc = fuse_reply_statfs(req, stat);			\
		if (__rc != 0)						\
			IOF_TRACE_ERROR(req,				\
					"fuse_reply_statfs returned %d:%s", \
					__rc, strerror(-__rc));		\
		IOF_TRACE_DOWN(req);					\
	} while (0)

#define IOF_FUSE_REPLY_IOCTL(req, gah_info, size)			\
	do {								\
		int __rc;						\
		IOF_TRACE_DEBUG(req, "Returning ioctl");		\
		__rc = fuse_reply_ioctl(req, 0, &gah_info, size);	\
		if (__rc != 0)						\
			IOF_TRACE_ERROR(req,				\
					"fuse_reply_ioctl returned %d:%s", \
					__rc, strerror(-__rc));		\
		IOF_TRACE_DOWN(req);					\
	} while (0)

struct ioc_request;

/**
 * IOF Request API.
 *
 * Set of callbacks invoked during the lifetime of a request.
 */
struct ioc_request_api {
	/** Called after RPC has been sent */
	void	(*on_send)(struct ioc_request *req);
	/** Called once, per request with the result */
	void	(*on_result)(struct ioc_request *req);
	/** Called on eviction, can re-send to new rank */
	int	(*on_evict)(struct ioc_request *req);
};

enum ioc_request_state {
	RS_INIT = 1,
	RS_RESET,
	RS_LIVE
};

/**
 * IOF Request descriptor.
 *
 */
struct ioc_request {
	/** Pointer to projection for this request. */
	struct iof_projection_info	*fsh;
	/** Pointer to the RPC for this request. */
	crt_rpc_t			*rpc;
	/** Fuse request for this IOF request, may be 0 */
	fuse_req_t			req;
	/** Callbacks to use for this request */
	const struct ioc_request_api	*ir_api;
	/** Error status of this request.
	 *
	 * This is a libc error number and is set before a call to
	 *  on_result
	 */
	int				rc;
	/** Request state.
	 *
	 * Used to ensure REQUEST_INIT()/REQUEST_RESET() have been invoked
	 * correctly.
	 */
	enum ioc_request_state		r_rs;

	/** List of requests.
	 *
	 * Used during failover to keep a list of requests that need to be
	 * actioned once failover is complete.
	 */
	d_list_t			r_list;
};

/** Initialise a request.  To be called once per request */
#define IOC_REQUEST_INIT(REQUEST, FSH)		\
	do {					\
		(REQUEST)->fsh = FSH;		\
		(REQUEST)->rpc = NULL;		\
		(REQUEST)->r_rs = RS_INIT;	\
		(REQUEST)->r_list.prev = NULL;	\
		(REQUEST)->r_list.next = NULL;	\
	} while (0)

/** Reset a request for re-use.  To be called before each use */
#define IOC_REQUEST_RESET(REQUEST)					\
	do {								\
		D_ASSERT((REQUEST)->r_rs == RS_INIT ||			\
			(REQUEST)->r_rs == RS_RESET ||			\
			(REQUEST)->r_rs == RS_LIVE);			\
		(REQUEST)->r_rs = RS_RESET;				\
		(REQUEST)->rc = 0;					\
	} while (0)

/**
 * Resolve request status.
 *
 * Correctly resolve the return codes and errors from the RPC response.
 * If the error code was already non-zero, it means an error occurred on
 * the client; do nothing. A non-zero error code in the RPC response
 * denotes a server error, in which case, set the status error code to EIO.
 */
#define IOC_REQUEST_RESOLVE(REQUEST, OUT)				\
	do {								\
		if (((OUT) != NULL) && (!(REQUEST)->rc)) {		\
			(REQUEST)->rc = (OUT)->rc;			\
			if ((OUT)->err)					\
				(REQUEST)->rc = EIO;			\
		}							\
	} while (0)

/**
 * Inode handle.
 *
 * Describes any entry in the projection that the kernel knows about, may
 * be a directory, file, symbolic link or anything else.
 */

struct ioc_inode_entry {
	/** The GAH for this inode */
	struct ios_gah	gah;
	/** stat structure for this inode.
	 * This will be valid, but out-of-date at any given moment in time,
	 * mainly used for the inode number and type.
	 */
	struct stat	stat;

	/** The name of the entry, relative to the parent.
	 * This would have been valid when the inode was first observed
	 * however may be incorrect at any point after that.  It may not
	 * even match the local kernels view of the projection as it is
	 * not updated on local rename requests.
	 */
	char		name[256];
	/** The parent inode of this entry.
	 *
	 * As with name this will be correct when created however may
	 * be incorrect at any point after that.  The inode does not hold
	 * a reference on the parent so the inode may not be valid.
	 */
	fuse_ino_t	parent;

	/** Hash table of inodes.
	 * All valid inodes are kept in a hash table, using the hash table
	 * locking.
	 */
	d_list_t	list;

	/** List of inodes.
	 * Populated during failover to be a list of inodes per parent
	 * directory.
	 */
	d_list_t	ie_ie_list;

	/** List of child inodes.
	 * Populated during failover to be a list of children for a directory
	 */
	d_list_t	ie_ie_children;

	/** List of open file handles for this inode.
	 * Populated during failover only.
	 */
	d_list_t	ie_fh_list;

	/** Reference counting for the inode.
	 * Used by the hash table callbacks
	 */
	ATOMIC uint	ref;

	/** Failover flag
	 * Set to true during failover if this inode should be migrated
	 */
	bool		failover;
};

/**
 * Directory handle.
 *
 * Describes a open directory, may be used for readdir() calls.
 */
struct iof_dir_handle {
	/** Request for opening the directory */
	struct ioc_request		open_req;
	/** Request for closing the directory */
	struct ioc_request		close_req;
	/** The GAH to use when accessing the directory */
	struct ios_gah			gah;
	/** Any RPC reference held across readdir() calls */
	crt_rpc_t			*rpc;
	/** Pointer to any retreived data from readdir() RPCs */
	struct iof_readdir_reply	*replies;
	int				reply_count;
	void				*replies_base;
	/** Set to True if the current batch of replies is the final one */
	int				last_replies;
	/** Set to 1 initially, but 0 if there is a unrecoverable error */
	int				handle_valid;
	/* Set to 0 if the server rejects the GAH at any point */
	ATOMIC int			gah_ok;
	/** The inode number of the directory */
	ino_t				inode_no;
	/** Endpoint for this directory handle */
	crt_endpoint_t			ep;
	/** List of directory handles */
	d_list_t			list;
};

/**
 * Open file handle.
 *
 * Describes a file open for reading/writing.
 */
struct iof_file_handle {
	/** The projection this file belongs to */
	struct iof_projection_info	*fs_handle;
	/** Common information for file handle, contains GAH and EP
	 * information.  This is shared between CNSS and IL code to allow
	 * use of some common code.
	 */
	struct iof_file_common		common;
	/** Boolean flag to indicate GAH is valid.
	 * Set to 1 when file is opened, however may be set to 0 either by
	 * ionss returning -DER_NONEXIST or by ionss failure
	 */
	ATOMIC int			gah_ok;
	/** Open RPC, precreated */
	crt_rpc_t			*open_rpc;
	/** Create RPC, precreated */
	crt_rpc_t			*creat_rpc;
	/** Release RPC, precreated */
	crt_rpc_t			*release_rpc;
	/** List of open files, stored in fs_handle->openfile_list */
	d_list_t			fh_of_list;

	/** List of open files for inode, stored in ino->ie_fh_list */
	d_list_t			fh_ino_list;
	/** The inode number of the file */
	ino_t				inode_no;
	/** A pre-allocated inode entry.  This is created as the struct is
	 * allocated and then used on a successful create() call.  Once
	 * the file handle is in use then this field will be NULL.
	 */
	struct ioc_inode_entry		*ie;
	/** Fuse req for open/create command.  Used by the RPC callback
	 * function to reply to a FUSE request
	 */
	fuse_req_t			open_req;
};

/* GAH ok manipulation macros. gah_ok is defined as a int but we're
 * using it as a bool and accessing it though the use of atomics.
 *
 * These macros work on both file and directory handles.
 *
 * TODO: Add gah_ok to inode entries.
 */

/** Set the GAH so that it's valid */
#define H_GAH_SET_VALID(OH) atomic_store_release(&OH->gah_ok, 1)

/** Set the GAH so that it's invalid.  Assumes it currently valid */
#define H_GAH_SET_INVALID(OH) atomic_store_release(&OH->gah_ok, 0)

/** Check if the handle is valid by reading the gah_ok field. */
#define H_GAH_IS_VALID(OH) atomic_load_consume(&OH->gah_ok)

/** Common request type.
 *
 * Used for getattr and close only.
 *
 * TODO: Rename this to something more specific.
 */
struct common_req {
	struct ioc_request		request;
	d_list_t			list;
};

/** Callback structure for inode migrate RPC.
 *
 * Used so migrate callback function has access to the filesystem handle.
 */
struct ioc_inode_migrate {
	struct ioc_inode_entry *im_ie;
	struct iof_projection_info *im_fsh;
};

/** Entry request type.
 *
 * Request for all RPC types that can return a new inode.
 */
struct entry_req {
	struct ioc_request		request;
	struct ioc_inode_entry		*ie;
	d_list_t			list;
	crt_opcode_t			opcode;
	struct iof_pool_type		*pool;
	char				*dest;
};

/* inode.c */

/* Convert from a inode to a GAH using the hash table */
int find_gah(struct iof_projection_info *, fuse_ino_t, struct ios_gah *);

/* Convert from a inode to a GAH and keep a reference using the hash table */
int find_gah_ref(struct iof_projection_info *, fuse_ino_t, struct ios_gah *);

/* Drop a reference on the GAH in the hash table */
void drop_ino_ref(struct iof_projection_info *, ino_t);

void ie_close(struct iof_projection_info *, struct ioc_inode_entry *);

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

void ioc_int_release(struct iof_file_handle *);

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
		  unsigned int, const void *, size_t, size_t);

void ioc_ll_setattr(fuse_req_t, fuse_ino_t, struct stat *, int,
		    struct fuse_file_info *);

void ioc_ll_symlink(fuse_req_t, const char *, fuse_ino_t, const char *);

void ioc_ll_fsync(fuse_req_t, fuse_ino_t, int, struct fuse_file_info *);

void iof_entry_cb(struct ioc_request *request);

#endif
