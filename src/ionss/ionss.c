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
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <mntent.h>
#include <getopt.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#ifdef __APPLE__
#include <sys/syslimits.h>
#endif

#include <fcntl.h>

#include "version.h"
#include "log.h"
#include "iof_common.h"
#include "iof_mntent.h"

#include "ionss.h"

#define IOF_MAX_PATH_LEN 4096
#define SHUTDOWN_BCAST_OP (0xFFF0)

static int shutdown;
static uint32_t	cnss_count;

static struct ios_base base;

#define VALIDATE_WRITE(fs_handle, out) \
	do {\
		if (!out) \
			break; \
		out->rc = 0; \
		if (!(fs_handle)) { \
			out->err = IOF_BAD_DATA; \
			break; \
		} \
		if (!IOF_IS_WRITEABLE((fs_handle)->flags)) { \
			IOF_LOG_INFO("Attempt to modify " \
				"Read-Only Projection!"); \
			(out)->rc = EROFS; \
		} \
	} while (0)

#define VALIDATE_ARGS_STR(rpc, in, out) \
	do {\
		if (in->fs_id >= base.projection_count) { \
			IOF_LOG_ERROR("Invalid Projection: " \
				      "[ID=%d]", in->fs_id); \
			out->err = IOF_BAD_DATA; \
			break; \
		} \
		if (!in->path) { \
			IOF_LOG_ERROR("No input path"); \
			out->err = IOF_ERR_CART; \
		} \
	} while (0)

#define VALIDATE_ARGS_STR2(rpc, in, out) \
	do {\
		if (in->fs_id >= base.projection_count) { \
			IOF_LOG_ERROR("Invalid Projection: " \
				      "[ID=%d]", in->fs_id); \
			out->err = IOF_BAD_DATA; \
			break; \
		} \
		if (!in->src || !in->dst) { \
			IOF_LOG_ERROR("Missing inputs."); \
			out->err = IOF_ERR_CART; \
		} \
	} while (0)

#define VALIDATE_ARGS_GAH(rpc, in, out, handle, handle_type) do {	\
		IOF_LOG_INFO("rpc(%p) " GAH_PRINT_STR,			\
			rpc,						\
			GAH_PRINT_VAL(in->gah));			\
		handle = ios_##handle_type##_find(&base, &in->gah);	\
		if (!handle) {						\
			out->err = IOF_GAH_INVALID;			\
		}							\
	} while (0)

#define VALIDATE_ARGS_GAH_FILE(rpc, in, out, handle) \
	VALIDATE_ARGS_GAH(rpc, in, out, handle, fh)

#define VALIDATE_ARGS_GAH_DIR(rpc, in, out, handle) \
	VALIDATE_ARGS_GAH(rpc, in, out, handle, dirh)

static void iof_register_default_handlers(void);

static void (*register_handlers
		  [IOF_PROTO_CLASSES])(void) = {
	iof_register_default_handlers
};

void shutdown_impl(void)
{
	IOF_LOG_DEBUG("Shutting Down");
	shutdown = 1;
}

/*
 * Call the shutdown implementation in the broadcast RPC callback in order
 * to ensure that the broadcast actually made it to all other IONSS ranks.
 */
static void
shutdown_bcast_cb(const struct crt_cb_info *cb_info)
{
	int rc;

	if (cb_info->cci_rc == 0) {
		shutdown_impl();
		return;
	}
	IOF_LOG_ERROR("Broadcast failed, rc = %u", cb_info->cci_rc);
	/* Retry in case of failure */
	rc = crt_req_send(cb_info->cci_rpc, shutdown_bcast_cb, NULL);
	if (rc)
		IOF_LOG_ERROR("Broadcast shutdown RPC not sent");
}

/*
 * Handle broadcast shutdown RPCs from other IONSS ranks.
 */
static void
shutdown_handler(crt_rpc_t *rpc)
{
	int rc = 0;

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, rc = %u", rc);
	shutdown_impl();
}

/*
 * The IONSS shuts down when the last CNSS detaches. In case there are
 * other running IONSS processes in the primary group, the local decision
 * to shut down must be broadcast to the others before exiting.
 */
static void
cnss_detach_handler(crt_rpc_t *rpc)
{
	int rc;
	crt_rpc_t *rpc_bcast = NULL;
	d_rank_list_t exclude_me = {  .rl_nr = { 1, 1 },
				      .rl_ranks = &base.my_rank };

	IOF_LOG_DEBUG("CNSS detach received (attached: %d)", cnss_count);
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, rc = %u", rc);

	/* Do nothing if there are more CNSS attached */
	if (--cnss_count)
		return;

	IOF_LOG_DEBUG("Last CNSS detached from Rank %d",
			base.my_rank);

	/* Call shutdown directly if this is the only IONSS running */
	if (base.num_ranks == 1) {
		shutdown_impl();
		return;
	}

	IOF_LOG_DEBUG("Broadcasting shutdown to %d IONSS",
			(base.num_ranks - 1));
	rc = crt_corpc_req_create(rpc->cr_ctx,
				  base.primary_group,
				  &exclude_me, SHUTDOWN_BCAST_OP,
				  NULL, NULL, 0,
				  crt_tree_topo(CRT_TREE_FLAT, 0),
				  &rpc_bcast);
	if (rc || !rpc_bcast) {
		IOF_LOG_ERROR("Could not create broadcast"
			      " shutdown request ret = %d", rc);
		return;
	}
	rc = crt_req_send(rpc_bcast, shutdown_bcast_cb, NULL);
	if (rc)
		IOF_LOG_ERROR("Broadcast shutdown RPC not sent");
}

static bool
fh_compare(struct chash_table *htable, d_list_t *rlink,
	   const void *key, unsigned int ksize)
{
	struct ionss_file_handle *fh = container_of(rlink,
						    struct ionss_file_handle,
						    clist);
	const struct ionss_mini_file *mf = key;

	if (fh->mf.inode_no != mf->inode_no)
		return false;

	return (fh->mf.flags == mf->flags);
}

static	uint32_t
fh_hash(struct chash_table *htable, const void *key, unsigned int ksize)
{
	const struct ionss_mini_file *mf = key;

	return mf->inode_no;
}

static chash_table_ops_t hops = {.hop_key_cmp = fh_compare,
				 .hop_key_hash = fh_hash,
};

/* Convert an absolute path into a real one, returning a pointer
 * to a string.
 *
 * This converts "/" into "." and for all other paths removes the leading /
 */
const char *iof_get_rel_path(const char *path)
{
	path++;
	if (path[0] == '\0')
		return ".";
	return (char *)path;
}

/*
 * Assemble local path.  Take the local projection directory and
 * concatename onto it a remote path.
 */
int iof_get_path(int id, const char *old_path, char *new_path)
{
	char *mnt;
	int ret;

	/*lookup mnt by ID in projection data structure*/
	if (id >= base.projection_count) {
		IOF_LOG_ERROR("Filesystem ID invalid");
		return IOF_BAD_DATA;
	}

	mnt = base.fs_list[id].mnt;

	ret = snprintf(new_path, IOF_MAX_PATH_LEN, "%s%s", mnt,
			old_path);
	if (ret > IOF_MAX_PATH_LEN)
		return IOF_ERR_OVERFLOW;
	IOF_LOG_DEBUG("New Path: %s", new_path);

	return IOF_SUCCESS;
}

static void
iof_getattr_handler(crt_rpc_t *rpc)
{
	struct iof_gah_string_in *in = crt_req_get(rpc);
	struct iof_getattr_out *out = crt_reply_get(rpc);
	struct ionss_file_handle *parent;
	struct stat stbuf = {0};
	int rc;

	VALIDATE_ARGS_GAH_FILE(rpc, in, out, parent);
	if (out->err)
		goto out;

	if (!in->path) {
		out->err = IOF_ERR_CART;
		goto out;
	}

	errno = 0;
	rc = fstatat(parent->fd,
		     iof_get_rel_path(in->path), &stbuf,
		     AT_SYMLINK_NOFOLLOW);
	if (rc)
		out->rc = errno;
	/* Deny access if this path is a mount point for another file system*/
	else if (parent->projection->dev_no != stbuf.st_dev)
		out->rc = EACCES;
	else
		d_iov_set(&out->stat, &stbuf, sizeof(struct stat));

out:

	IOF_LOG_DEBUG("path %s result err %d rc %d",
		      in->path, out->err, out->rc);

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, rc = %u", rc);

	if (parent)
		ios_fh_decref(parent, 1);
}

/*
 * Given a GAH, return the file attributes.
 *
 * Although a GAH may represent either a file or a directory, this function
 * will only be called on regular files that are already open. The kernel sets
 * the FUSE_GETATTR_FH exclusively in case of regular open files.
 * In the absence of the flag, FUSE passes a NULL fuse_file_info pointer to
 * the 'getattr' implementation on the client which routes the call to
 * iof_getattr_handler instead. Thus it is safe to assume that this function
 * will never be called on a directory.
 */
static void
iof_getattr_gah_handler(crt_rpc_t *rpc)
{
	struct iof_gah_in *in = crt_req_get(rpc);
	struct iof_getattr_out *out = crt_reply_get(rpc);
	struct ionss_file_handle *handle;
	struct stat stbuf = {0};
	int rc;

	VALIDATE_ARGS_GAH_FILE(rpc, in, out, handle);
	if (out->err)
		goto out;

	errno = 0;
	rc = fstat(handle->fd, &stbuf);

	if (rc)
		out->rc = errno;
	else
		d_iov_set(&out->stat, &stbuf, sizeof(struct stat));

out:
	IOF_LOG_DEBUG("result err %d rc %d",
		      out->err, out->rc);

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, rc = %u", rc);

	if (handle)
		ios_fh_decref(handle, 1);
}

static void
iof_opendir_handler(crt_rpc_t *rpc)
{
	struct iof_string_in *in = crt_req_get(rpc);
	struct iof_opendir_out *out = crt_reply_get(rpc);
	struct ionss_dir_handle *local_handle;
	int rc;
	int fd;

	VALIDATE_ARGS_STR(rpc, in, out);
	if (out->err)
		goto out;

	IOF_LOG_DEBUG("Opening path %s", in->path);

	errno = 0;
	fd = openat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->path),
		    O_DIRECTORY | O_RDONLY);

	if (fd == -1) {
		out->rc = errno;
		goto out;
	}

	local_handle = malloc(sizeof(*local_handle));
	if (!local_handle) {
		IOF_LOG_ERROR("Could not allocate handle");
		out->err = IOF_ERR_NOMEM;
		close(fd);
		goto out;
	}

	local_handle->fd = fd;
	local_handle->h_dir = fdopendir(local_handle->fd);
	local_handle->offset = 0;

	rc = ios_gah_allocate(base.gs, &out->gah, 0, 0, local_handle);
	if (rc != IOS_SUCCESS) {
		closedir(local_handle->h_dir);
		free(local_handle);
		out->err = IOF_ERR_INTERNAL;
		goto out;
	}

	IOF_LOG_INFO("Handle %p " GAH_PRINT_FULL_STR, local_handle,
		     GAH_PRINT_FULL_VAL(out->gah));

out:
	IOF_LOG_DEBUG("path %s result err %d rc %d",
		      in->path, out->err, out->rc);

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, rc = %u", rc);
}

int iof_readdir_bulk_cb(const struct crt_bulk_cb_info *cb_info)
{
	struct iof_readdir_out *out = crt_reply_get(cb_info->bci_bulk_desc->bd_rpc);
	d_iov_t iov = {0};
	d_sg_list_t sgl = {0};
	int rc;

	if (cb_info->bci_rc) {
		out->err = IOF_ERR_CART;
		goto out;
	}

	sgl.sg_iovs = &iov;
	sgl.sg_nr.num = 1;

	rc = crt_bulk_access(cb_info->bci_bulk_desc->bd_local_hdl, &sgl);
	if (rc) {
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_DEBUG("Freeing buffer %p", iov.iov_buf);
	free(iov.iov_buf);

	rc = crt_bulk_free(cb_info->bci_bulk_desc->bd_local_hdl);
	if (rc)
		out->err = IOF_ERR_CART;

out:
	rc = crt_reply_send(cb_info->bci_bulk_desc->bd_rpc);

	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	rc = crt_req_decref(cb_info->bci_bulk_desc->bd_rpc);

	return 0;
}

/*
 * Read dirent from a directory and reply to the origin.
 *
 * TODO:
 * Use readdir_r().  This code is not thread safe.
 * Parse GAH better.  If a invalid GAH is passed then it's handled but we
 * really should pass this back to the client properly so it doesn't retry.
 *
 */
static void
iof_readdir_handler(crt_rpc_t *rpc)
{
	struct iof_readdir_in *in = crt_req_get(rpc);
	struct iof_readdir_out *out = crt_reply_get(rpc);
	struct ionss_dir_handle *handle;
	struct iof_readdir_reply *replies = NULL;
	int max_reply_count;
	struct dirent *dir_entry;
	struct crt_bulk_desc bulk_desc = {0};
	crt_bulk_t local_bulk_hdl = {0};
	d_sg_list_t sgl = {0};
	d_iov_t iov = {0};
	d_size_t len = 0;
	int reply_idx = 0;
	int rc;

	VALIDATE_ARGS_GAH_DIR(rpc, in, out, handle);

	IOF_LOG_INFO(GAH_PRINT_STR " offset %zi rpc %p",
		     GAH_PRINT_VAL(in->gah), in->offset, rpc);

	if (out->err)
		goto out;

	if (in->bulk) {
		rc = crt_bulk_get_len(in->bulk, &len);
		if (rc || !len) {
			out->err = IOF_ERR_CART;
			goto out;
		}

		if (len > base.max_readdir) {
			IOF_LOG_WARNING("invalid readdir size %zi", len);
			len = base.max_readdir;
		}

		max_reply_count = len / sizeof(struct iof_readdir_reply);
	} else {
		IOF_LOG_INFO("No bulk descriptor, replying inline");
		max_reply_count = IONSS_READDIR_ENTRIES_PER_RPC;
		len = sizeof(struct iof_readdir_reply) * max_reply_count;
	}

	IOF_LOG_DEBUG("max_replies %d len %zi bulk %p", max_reply_count, len,
		      in->bulk);

	replies = calloc(max_reply_count, sizeof(*replies));
	if (!replies) {
		out->err = IOF_ERR_NOMEM;
		goto out;
	}

	if (handle->offset != in->offset) {
		IOF_LOG_DEBUG("Changing offset %zi %zi",
			      handle->offset, in->offset);
		seekdir(handle->h_dir, in->offset);
		handle->offset = in->offset;
	}

	reply_idx = 0;
	do {
		errno = 0;
		dir_entry = readdir(handle->h_dir);

		if (!dir_entry) {
			if (errno == 0) {
				IOF_LOG_DEBUG("Last entry %d", reply_idx);
				/* End of directory */
				out->last = 1;
			} else {
				/* An error occoured */
				replies[reply_idx].read_rc = errno;
				reply_idx++;
			}

			goto out;
		}

		if (strncmp(".", dir_entry->d_name, 2) == 0)
			continue;

		if (strncmp("..", dir_entry->d_name, 3) == 0)
			continue;

		handle->offset = telldir(handle->h_dir);
		replies[reply_idx].nextoff = handle->offset;

		/* TODO: Check this */
		strncpy(replies[reply_idx].d_name, dir_entry->d_name, NAME_MAX);

		IOF_LOG_DEBUG("File %s nextoff %zi", dir_entry->d_name,
			      handle->offset);

		errno = 0;
		rc = fstatat(handle->fd,
			     replies[reply_idx].d_name,
			     &replies[reply_idx].stat,
			     AT_SYMLINK_NOFOLLOW);
		if (rc != 0)
			replies[reply_idx].stat_rc = errno;

		reply_idx++;
	} while (reply_idx < (max_reply_count));

out:

	IOF_LOG_INFO("Sending %d replies", reply_idx);

	if (reply_idx > IONSS_READDIR_ENTRIES_PER_RPC) {
		rc = crt_req_addref(rpc);
		if (rc) {
			out->err = IOF_ERR_CART;
			goto out;
		}

		iov.iov_len = sizeof(struct iof_readdir_reply) * reply_idx;
		iov.iov_buf = replies;
		iov.iov_buf_len = sizeof(struct iof_readdir_reply) * reply_idx;
		sgl.sg_iovs = &iov;
		sgl.sg_nr.num = 1;

		rc = crt_bulk_create(rpc->cr_ctx, &sgl, CRT_BULK_RO,
				     &local_bulk_hdl);
		if (rc) {
			out->err = IOF_ERR_CART;
			goto out;
		}

		bulk_desc.bd_rpc = rpc;
		bulk_desc.bd_bulk_op = CRT_BULK_PUT;
		bulk_desc.bd_remote_hdl = in->bulk;
		bulk_desc.bd_local_hdl = local_bulk_hdl;
		bulk_desc.bd_len = sizeof(struct iof_readdir_reply) * reply_idx;

		out->bulk_count = reply_idx;

		rc = crt_bulk_transfer(&bulk_desc, iof_readdir_bulk_cb,
				       NULL, NULL);
		if (rc) {
			out->err = IOF_ERR_CART;
			goto out;
		}

		return;
	} else if (reply_idx) {
		out->iov_count = reply_idx;
		d_iov_set(&out->replies, &replies[0],
			  sizeof(struct iof_readdir_reply) * reply_idx);
	}

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR(" response not sent, rc = %u", rc);

	if (replies)
		free(replies);
}

static void
iof_closedir_handler(crt_rpc_t *rpc)
{
	struct iof_gah_in *in = crt_req_get(rpc);
	struct ionss_dir_handle *handle = NULL;
	int rc;

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(in->gah));

	rc = ios_gah_get_info(base.gs, &in->gah, (void **)&handle);
	if (rc != IOS_SUCCESS)
		IOF_LOG_DEBUG("Failed to load DIR* from gah %p %d",
			      &in->gah, rc);

	if (handle) {
		IOF_LOG_DEBUG("Closing %p", handle->h_dir);
		rc = closedir(handle->h_dir);
		if (rc != 0)
			IOF_LOG_DEBUG("Failed to close directory %p",
				      handle->h_dir);
		free(handle);
	}

	ios_gah_deallocate(base.gs, &in->gah);

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, rc = %u", rc);
}

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

static void
iof_open_handler(crt_rpc_t *rpc)
{
	struct iof_open_in	*in = crt_req_get(rpc);
	struct iof_open_out	*out = crt_reply_get(rpc);
	struct ionss_file_handle *handle = NULL;
	struct ios_projection	*projection = NULL;
	struct ionss_mini_file	mf;
	struct stat		stbuf = {0};
	d_list_t		*rlink;
	int fd;
	int rc;

	VALIDATE_ARGS_STR(rpc, in, out);
	if (out->err)
		goto out;

	if (in->flags & O_WRONLY || in->flags & O_RDWR) {
		VALIDATE_WRITE(&base.fs_list[in->fs_id], out);
		if (out->err || out->rc)
			goto out;
	}

	/* in->fs_id will have been verified by the VALIDATE_ARGS_STR call
	 * above
	 */
	projection = &base.projection_array[in->fs_id];

	IOF_LOG_DEBUG("path %s flags 0%o",
		      in->path, in->flags);

	errno = 0;
	fd = openat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->path), in->flags);
	if (fd == -1) {
		out->rc = errno;
		goto out;
	}

	rc = fstat(fd, &stbuf);
	if (rc) {
		out->rc = errno;
		close(fd);
		goto out;
	}

	pthread_mutex_lock(&projection->lock);

	/* Check if a pre-existing GAH can be reused for this open call.
	 *
	 * To be re-used the file must be the same (inode number must match),
	 * some of the open flags must match.  Currently this code checks all
	 * flags however this could be relaxed in future.
	 *
	 * If the handle can be re-used then close the new file descriptor
	 * and take a reference on the existing GAH.
	 */
	mf.flags = in->flags;
	mf.inode_no = stbuf.st_ino;

	rlink = chash_rec_find(&projection->file_ht, &mf, sizeof(mf));
	if (rlink) {
		handle = container_of(rlink, struct ionss_file_handle, clist);

		close(fd);
		atomic_fetch_add(&handle->ref, 1);

	}

	if (!handle) {
		rc = ios_fh_alloc(projection, &handle);
		if (rc || !handle) {
			out->err = IOF_ERR_NOMEM;
			close(fd);
			pthread_mutex_unlock(&projection->lock);
			goto out;
		}

		handle->fd = fd;
		handle->projection = projection;
		handle->mf.flags = mf.flags;
		handle->mf.inode_no = mf.inode_no;

		chash_rec_insert(&projection->file_ht,
				 &mf,
				 sizeof(mf),
				 &handle->clist,
				 0);
	}

	pthread_mutex_unlock(&projection->lock);

	out->gah = handle->gah;

out:
	IOF_LOG_DEBUG("path %s flags 0%o ", in->path, in->flags);

	LOG_FLAGS(handle, in->flags);

	IOF_LOG_INFO("%p path %s result err %d rc %d",
		     handle, in->path, out->err, out->rc);

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	if (projection)
		iof_pool_restock(projection->fh_pool);
}

static void
iof_create_handler(crt_rpc_t *rpc)
{
	struct iof_create_in	*in = crt_req_get(rpc);
	struct iof_open_out	*out = crt_reply_get(rpc);
	struct ionss_file_handle *handle = NULL;
	struct ios_projection	*projection = NULL;
	struct ionss_mini_file	mf;
	struct stat		stbuf = {0};
	d_list_t		*rlink;
	int fd;
	int rc;

	VALIDATE_ARGS_STR(rpc, in, out);
	if (out->err)
		goto out;

	VALIDATE_WRITE(&base.fs_list[in->fs_id], out);
	if (out->err || out->rc)
		goto out;

	/* in->fs_id will have been verified by the VALIDATE_ARGS_STR call
	 * above
	 */
	projection = &base.projection_array[in->fs_id];

	IOF_LOG_DEBUG("path %s flags 0%o mode 0%o",
		      in->path, in->flags, in->mode);

	errno = 0;
	fd = openat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->path), in->flags,
		    in->mode);
	if (fd == -1) {
		out->rc = errno;
		goto out;
	}

	rc = fstat(fd, &stbuf);
	if (rc) {
		out->rc = errno;
		close(fd);
		goto out;
	}

	pthread_mutex_lock(&projection->lock);

	/* Check if a pre-existing GAH can be reused for this open call.
	 *
	 * To be re-used the file must be the same (inode number must match),
	 * some of the open flags must match.  Currently this code checks all
	 * flags however this could be relaxed in future.
	 *
	 * If the handle can be re-used then close the new file descriptor
	 * and take a reference on the existing GAH.
	 */
	mf.flags = in->flags;
	mf.inode_no = stbuf.st_ino;

	rlink = chash_rec_find(&projection->file_ht, &mf, sizeof(mf));
	if (rlink) {
		handle = container_of(rlink, struct ionss_file_handle, clist);

		close(fd);
		atomic_fetch_add(&handle->ref, 1);

	}

	if (!handle) {
		rc = ios_fh_alloc(projection, &handle);
		if (rc || !handle) {
			out->err = IOF_ERR_NOMEM;
			close(fd);
			pthread_mutex_unlock(&projection->lock);
			goto out;
		}

		handle->fd = fd;
		handle->projection = projection;
		handle->mf.flags = mf.flags;
		handle->mf.inode_no = mf.inode_no;

		chash_rec_insert(&projection->file_ht, &mf, sizeof(mf),
				  &handle->clist, 0);
	}

	pthread_mutex_unlock(&projection->lock);

	out->gah = handle->gah;

out:
	IOF_LOG_DEBUG("path %s flags 0%o mode 0%o 0%o", in->path, in->flags,
		      in->mode & S_IFREG, in->mode & ~S_IFREG);

	LOG_FLAGS(handle, in->flags);
	LOG_MODES(handle, in->mode);

	IOF_LOG_INFO("%p path %s result err %d rc %d",
		     handle, in->path, out->err, out->rc);

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	if (projection)
		iof_pool_restock(projection->fh_pool);
}

/* Handle a close from a client.
 * For close RPCs there is no reply so simply ack the RPC first
 * and then do the work off the critical path.
 */
static void
iof_close_handler(crt_rpc_t *rpc)
{
	struct iof_gah_in *in = crt_req_get(rpc);
	struct ionss_file_handle *handle;
	int rc;

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	handle = ios_fh_find(&base, &in->gah);

	if (handle)
		ios_fh_decref(handle, 2);
}

static void
iof_fsync_handler(crt_rpc_t *rpc)
{
	struct iof_gah_in *in = crt_req_get(rpc);
	struct iof_status_out *out = crt_reply_get(rpc);
	struct ionss_file_handle *handle;
	int rc;

	VALIDATE_ARGS_GAH_FILE(rpc, in, out, handle);
	if (out->err)
		goto out;

	VALIDATE_WRITE(handle->projection, out);
	if (out->err || out->rc)
		goto out;

	errno = 0;
	rc = fsync(handle->fd);
	if (rc)
		out->rc = errno;

out:
	IOF_LOG_DEBUG("result err %d rc %d",
		      out->err, out->rc);

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	if (handle)
		ios_fh_decref(handle, 1);
}

static void
iof_fdatasync_handler(crt_rpc_t *rpc)
{
	struct iof_gah_in *in = crt_req_get(rpc);
	struct iof_status_out *out = crt_reply_get(rpc);
	struct ionss_file_handle *handle;
	int rc;

	VALIDATE_ARGS_GAH_FILE(rpc, in, out, handle);
	if (out->err)
		goto out;

	VALIDATE_WRITE(handle->projection, out);
	if (out->err || out->rc)
		goto out;

	errno = 0;
	rc = fdatasync(handle->fd);
	if (rc)
		out->rc = errno;

out:
	IOF_LOG_DEBUG("result err %d rc %d",
		      out->err, out->rc);

	rc = crt_reply_send(rpc);

	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	if (handle)
		ios_fh_decref(handle, 1);
}

static void
iof_read_handler(crt_rpc_t *rpc)
{
	struct iof_read_in *in = crt_req_get(rpc);
	struct iof_data_out *out = crt_reply_get(rpc);
	struct ionss_file_handle *handle;
	void *data = NULL;
	size_t bytes_read;
	int rc;

	VALIDATE_ARGS_GAH_FILE(rpc, in, out, handle);
	if (out->err)
		goto out;

	if (in->len > base.max_iov_read) {
		IOF_LOG_WARNING("Invalid read, too large");
		out->err = IOF_ERR_INTERNAL;
		goto out;
	}

	IOF_LOG_DEBUG("Reading from %d", handle->fd);

	data = malloc(in->len);
	if (!data) {
		out->err = IOF_ERR_NOMEM;
		goto out;
	}

	errno = 0;
	bytes_read = pread(handle->fd, data, in->len, in->base);
	if (bytes_read == -1)
		out->rc = errno;
	else
		d_iov_set(&out->data, data, bytes_read);

out:
	rc = crt_reply_send(rpc);

	if (rc)
		IOF_LOG_ERROR("response not sent, rc = %u", rc);

	if (data)
		free(data);

	if (handle)
		ios_fh_decref(handle, 1);
}

static int iof_read_bulk_cb(const struct crt_bulk_cb_info *cb_info);
static void iof_process_read_bulk(struct ionss_active_read *ard,
				  struct ionss_read_req_desc *rrd);

void iof_read_check_and_send(struct ios_projection *projection)
{
	struct ionss_read_req_desc *rrd;
	struct ionss_active_read *ard;

	pthread_mutex_lock(&projection->lock);
	if (d_list_empty(&projection->read_list)) {
		projection->current_read_count--;
		IOF_LOG_DEBUG("Dropping read slot (%d/%d)",
			      projection->current_read_count,
			      projection->max_read_count);
		pthread_mutex_unlock(&projection->lock);
		return;
	}

	ard = iof_pool_acquire(projection->ar_pool);
	if (!ard) {
		projection->current_read_count--;
		IOF_LOG_WARNING("No ARD slot available (%d/%d)",
				projection->current_read_count,
				projection->max_read_count);
		pthread_mutex_unlock(&projection->lock);
		return;
	}

	rrd = d_list_entry(projection->read_list.next,
			     struct ionss_read_req_desc, list);

	d_list_del(&rrd->list);

	IOF_LOG_DEBUG("Submiting new read %p (%d/%d)", rrd,
		      projection->current_read_count,
		      projection->max_read_count);

	pthread_mutex_unlock(&projection->lock);
	iof_process_read_bulk(ard, rrd);
}

/* Process a read request
 *
 * This function processes a single rrd and either submits a bulk write with
 * the result of completes and frees the request
 */
static void
iof_process_read_bulk(struct ionss_active_read *ard,
		      struct ionss_read_req_desc *rrd)
{
	struct ionss_file_handle *handle = rrd->handle;
	struct ios_projection *projection = rrd->handle->projection;
	struct iof_read_bulk_out *out = rrd->out;
	struct crt_bulk_desc bulk_desc = {0};
	int rc;

	IOF_LOG_DEBUG("Processing read rrd %p", rrd);

	rrd->ard = ard;

	IOF_LOG_DEBUG("Processing read rrd %p ard %p", rrd, rrd->ard);

	IOF_LOG_DEBUG("Reading from %d", handle->fd);

	errno = 0;
	rrd->ard->read_len = pread(handle->fd, rrd->ard->buf, rrd->req_len,
				   rrd->in->base);
	if (rrd->ard->read_len == -1) {
		out->rc = errno;
		goto out;
	} else if (rrd->ard->read_len <= base.max_iov_read) {
		out->iov_len = rrd->ard->read_len;
		d_iov_set(&out->data, rrd->ard->buf, rrd->ard->read_len);
		goto out;
	}

	bulk_desc.bd_rpc = rrd->rpc;
	bulk_desc.bd_bulk_op = CRT_BULK_PUT;
	bulk_desc.bd_remote_hdl = rrd->in->bulk;
	bulk_desc.bd_local_hdl = rrd->ard->local_bulk_handle;
	bulk_desc.bd_len = rrd->ard->read_len;

	IOF_LOG_DEBUG("Sending bulk " GAH_PRINT_STR,
		      GAH_PRINT_VAL(rrd->in->gah));

	rc = crt_bulk_transfer(&bulk_desc, iof_read_bulk_cb, rrd, NULL);
	if (rc) {
		out->err = IOF_ERR_CART;
		goto out;
	}

	/* Do not call crt_reply_send() in this case as it'll be done in the
	 * bulk handler however it's safe to drop the handle as the read
	 * has completed at this point.
	 */

	ios_fh_decref(handle, 1);

	return;

out:

	rc = crt_reply_send(rrd->rpc);

	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	rc = crt_req_decref(rrd->rpc);
	if (rc)
		IOF_LOG_ERROR("decref failed, ret = %u", rc);

	iof_pool_release(projection->ar_pool, rrd->ard);
	free(rrd);

	ios_fh_decref(handle, 1);

	iof_read_check_and_send(projection);
}

/* Completion callback for bulk read request
 *
 * This function is called when a put to the client has completed for a bulk
 * write.
 */
static int
iof_read_bulk_cb(const struct crt_bulk_cb_info *cb_info)
{
	struct ionss_read_req_desc *rrd = cb_info->bci_arg;
	struct ios_projection *projection = rrd->handle->projection;
	int rc;

	if (cb_info->bci_rc)
		rrd->out->err = IOF_ERR_CART;
	else
		rrd->out->bulk_len = rrd->ard->read_len;

	rc = crt_reply_send(rrd->rpc);

	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	rc = crt_req_decref(rrd->rpc);
	if (rc)
		IOF_LOG_ERROR("decref failed, ret = %u", rc);

	iof_pool_release(projection->ar_pool, rrd->ard);
	free(rrd);

	iof_read_check_and_send(projection);
	return 0;
}

/*
 * The target of a bulk_read RPC from a client, replies using bulk data.
 *
 * Pulls the RPC off the network, allocates a read request descriptor, checks
 * active read count and either submits the read or queues it for later.
 */
static void
iof_read_bulk_handler(crt_rpc_t *rpc)
{
	struct iof_read_bulk_in *in = crt_req_get(rpc);
	struct iof_read_bulk_out *out = crt_reply_get(rpc);
	struct ionss_file_handle *handle;
	struct ionss_read_req_desc *rrd = NULL;
	struct ionss_active_read *ard;

	struct ios_projection *projection;
	int rc;

	VALIDATE_ARGS_GAH_FILE(rpc, in, out, handle);
	if (out->err)
		goto out;

	if (in->len > base.max_read) {
		IOF_LOG_WARNING("Invalid read, too large");
		out->err = IOF_ERR_INTERNAL;
		goto out;
	}

	rrd = calloc(1, sizeof(*rrd));
	if (!rrd) {
		out->err = IOF_ERR_NOMEM;
		goto out;
	}

	rrd->rpc = rpc;
	rrd->in = in;
	rrd->out = out;
	rrd->req_len = in->len;
	rrd->handle = handle;

	rc = crt_req_addref(rpc);
	if (rc) {
		out->err = IOF_ERR_CART;
		goto out;
	}

	projection = handle->projection;

	pthread_mutex_lock(&projection->lock);

	/* Try and acquire a active read descriptor, if one is available then
	 * start the read, else add it to the list
	 */
	ard = iof_pool_acquire(projection->ar_pool);
	if (ard) {
		projection->current_read_count++;
		IOF_LOG_DEBUG("Injecting new read %p (%d/%d)", rrd,
			      projection->current_read_count,
			      projection->max_read_count);
		pthread_mutex_unlock(&projection->lock);
		iof_process_read_bulk(ard, rrd);
	} else {
		d_list_add_tail(&rrd->list, &projection->read_list);
		pthread_mutex_unlock(&projection->lock);
	}

	return;
out:

	IOF_LOG_DEBUG("Failed to read %d " GAH_PRINT_STR, out->err,
		      GAH_PRINT_VAL(in->gah));
	rc = crt_reply_send(rpc);

	if (rc)
		IOF_LOG_ERROR("response not sent, rc = %u", rc);

	if (rrd)
		free(rrd);

	if (handle)
		ios_fh_decref(handle, 1);
}

static void
iof_rename_handler(crt_rpc_t *rpc)
{
	struct iof_two_string_in *in = crt_req_get(rpc);
	struct iof_status_out *out = crt_reply_get(rpc);
	int rc;

	VALIDATE_ARGS_STR2(rpc, in, out);
	if (out->err)
		goto out;

	VALIDATE_WRITE(&base.fs_list[in->fs_id], out);
	if (out->err || out->rc)
		goto out;

	rc = renameat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->src),
		      ID_TO_FD(in->fs_id), iof_get_rel_path(in->dst));

	if (rc)
		out->rc = errno;

out:
	IOF_LOG_DEBUG("src %s dst %s result err %d rc %d",
		      in->src, in->dst, out->err, out->rc);

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);
}

static void
iof_symlink_handler(crt_rpc_t *rpc)
{
	struct iof_two_string_in *in = crt_req_get(rpc);
	struct iof_status_out *out = crt_reply_get(rpc);
	int rc;

	VALIDATE_ARGS_STR2(rpc, in, out);
	if (out->err)
		goto out;

	VALIDATE_WRITE(&base.fs_list[in->fs_id], out);
	if (out->err || out->rc)
		goto out;

	rc = symlinkat(in->dst, ID_TO_FD(in->fs_id),
		       iof_get_rel_path(in->src));

	if (rc)
		out->rc = errno;

out:
	IOF_LOG_DEBUG("src %s dst %s result err %d rc %d",
		      in->src, in->dst, out->err, out->rc);

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);
}

static void
iof_mkdir_handler(crt_rpc_t *rpc)
{
	struct iof_create_in *in = crt_req_get(rpc);
	struct iof_status_out *out = crt_reply_get(rpc);
	int rc;

	VALIDATE_ARGS_STR(rpc, in, out);
	if (out->err)
		goto out;

	VALIDATE_WRITE(&base.fs_list[in->fs_id], out);
	if (out->err || out->rc)
		goto out;

	errno = 0;
	rc = mkdirat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->path), in->mode);

	if (rc)
		out->rc = errno;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);
}

/* This function needs additional checks to handle longer links.
 *
 * There is a upper limit on the lenth FUSE has provided in the CNSS but this
 * function should ensure that it's not introducing further restricions.
 */
static void
iof_readlink_handler(crt_rpc_t *rpc)
{
	struct iof_string_in *in = crt_req_get(rpc);
	struct iof_string_out *out = crt_reply_get(rpc);
	char reply[IOF_MAX_PATH_LEN] = {0};
	int rc;

	VALIDATE_ARGS_STR(rpc, in, out);
	if (out->err)
		goto out;

	errno = 0;
	rc = readlinkat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->path),
			reply, IOF_MAX_PATH_LEN);

	if (rc < 0)
		out->rc = errno;
	else
		out->path = (d_string_t)reply;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);
}

static void
iof_truncate_handler(crt_rpc_t *rpc)
{
	struct iof_truncate_in *in = crt_req_get(rpc);
	struct iof_status_out *out = crt_reply_get(rpc);
	char new_path[IOF_MAX_PATH_LEN];
	int rc;

	VALIDATE_ARGS_STR(rpc, in, out);
	if (out->err)
		goto out;

	VALIDATE_WRITE(&base.fs_list[in->fs_id], out);
	if (out->err || out->rc)
		goto out;

	rc = iof_get_path(in->fs_id, in->path, &new_path[0]);
	if (rc) {
		IOF_LOG_ERROR("could not construct filesystem path, rc = %d",
			      rc);
		out->err = rc;
		goto out;
	}

	errno = 0;
	rc = truncate(new_path, in->len);

	if (rc)
		out->rc = errno;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);
}

static void
iof_ftruncate_handler(crt_rpc_t *rpc)
{
	struct iof_ftruncate_in *in = crt_req_get(rpc);
	struct iof_status_out *out = crt_reply_get(rpc);
	struct ionss_file_handle *handle;
	int rc;

	VALIDATE_ARGS_GAH_FILE(rpc, in, out, handle);
	if (out->err)
		goto out;

	VALIDATE_WRITE(handle->projection, out);
	if (out->err || out->rc)
		goto out;

	errno = 0;
	rc = ftruncate(handle->fd, in->len);

	if (rc)
		out->rc = errno;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	if (handle)
		ios_fh_decref(handle, 1);
}

static void
iof_chmod_handler(crt_rpc_t *rpc)
{
	struct iof_chmod_in *in = crt_req_get(rpc);
	struct iof_status_out *out = crt_reply_get(rpc);
	int rc;

	VALIDATE_ARGS_STR(rpc, in, out);
	if (out->err)
		goto out;

	VALIDATE_WRITE(&base.fs_list[in->fs_id], out);
	if (out->err || out->rc)
		goto out;

	IOF_LOG_INFO("Setting mode for %s to 0%o", in->path, in->mode);

	/* The documentation for fchmodat() references AT_SYMLINK_NOFOLLOW
	 * however then says it is not supported.  This seems to match chmod
	 * itself however could be a bug
	 */
	errno = 0;
	rc = fchmodat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->path), in->mode,
		      0);

	if (rc)
		out->rc = errno;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

}

static void iof_chmod_gah_handler(crt_rpc_t *rpc)
{
	struct iof_chmod_gah_in *in = crt_req_get(rpc);
	struct iof_status_out *out = crt_reply_get(rpc);
	struct ionss_file_handle *handle;
	int rc;

	VALIDATE_ARGS_GAH_FILE(rpc, in, out, handle);
	if (out->err)
		goto out;

	VALIDATE_WRITE(handle->projection, out);
	if (out->err || out->rc)
		goto out;

	errno = 0;
	rc = fchmod(handle->fd, in->mode);

	if (rc)
		out->rc = errno;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	if (handle)
		ios_fh_decref(handle, 1);
}

static void
iof_rmdir_handler(crt_rpc_t *rpc)
{
	struct iof_string_in *in = crt_req_get(rpc);
	struct iof_status_out *out = crt_reply_get(rpc);
	int rc;

	VALIDATE_ARGS_STR(rpc, in, out);
	if (out->err)
		goto out;

	VALIDATE_WRITE(&base.fs_list[in->fs_id], out);
	if (out->err || out->rc)
		goto out;

	errno = 0;
	rc = unlinkat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->path),
		      AT_REMOVEDIR);

	if (rc)
		out->rc = errno;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);
}

static void iof_unlink_handler(crt_rpc_t *rpc)
{
	struct iof_string_in *in = crt_req_get(rpc);
	struct iof_status_out *out = crt_reply_get(rpc);
	int rc;

	VALIDATE_ARGS_STR(rpc, in, out);
	if (out->err)
		goto out;

	VALIDATE_WRITE(&base.fs_list[in->fs_id], out);
	if (out->err || out->rc)
		goto out;

	errno = 0;
	rc = unlinkat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->path), 0);

	if (rc)
		out->rc = errno;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);
}

static void
iof_write_direct_handler(crt_rpc_t *rpc)
{
	struct iof_write_in *in = crt_req_get(rpc);
	struct iof_write_out *out = crt_reply_get(rpc);
	struct ionss_file_handle *handle;
	size_t bytes_written;
	int rc;

	VALIDATE_ARGS_GAH_FILE(rpc, in, out, handle);
	if (out->err)
		goto out;

	VALIDATE_WRITE(handle->projection, out);
	if (out->err || out->rc)
		goto out;

	bytes_written = pwrite(handle->fd, in->data.iov_buf, in->data.iov_len,
			       in->base);
	if (bytes_written == -1)
		out->rc = errno;
	else
		out->len = bytes_written;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	if (handle)
		ios_fh_decref(handle, 1);
}

int iof_write_bulk(const struct crt_bulk_cb_info *cb_info)
{
	struct iof_write_bulk *in = crt_req_get(cb_info->bci_bulk_desc->bd_rpc);
	struct iof_write_out *out = crt_reply_get(cb_info->bci_bulk_desc->bd_rpc);
	size_t bytes_written;
	struct ionss_file_handle *handle = NULL;
	d_iov_t iov = {0};
	d_sg_list_t sgl = {0};

	int rc;

	if (cb_info->bci_rc) {
		out->err = IOF_ERR_CART;
		goto out;
	}

	handle = ios_fh_find(&base, &in->gah);
	if (!handle) {
		out->err = IOF_GAH_INVALID;
		goto out;
	}
	VALIDATE_WRITE(handle->projection, out);
	if (out->err || out->rc)
		goto out;

	sgl.sg_iovs = &iov;
	sgl.sg_nr.num = 1;

	rc = crt_bulk_access(cb_info->bci_bulk_desc->bd_local_hdl, &sgl);
	if (rc) {
		out->err = IOF_ERR_CART;
		goto out;
	}

	errno = 0;
	bytes_written = pwrite(handle->fd, iov.iov_buf, iov.iov_len, in->base);
	if (bytes_written == -1)
		out->rc = errno;
	else
		out->len = bytes_written;

	free(iov.iov_buf);
	rc = crt_bulk_free(cb_info->bci_bulk_desc->bd_local_hdl);
	if (rc)
		out->err = IOF_ERR_CART;

out:
	rc = crt_reply_send(cb_info->bci_bulk_desc->bd_rpc);

	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	rc = crt_req_decref(cb_info->bci_bulk_desc->bd_rpc);

	if (rc)
		IOF_LOG_ERROR("Unable to drop reference, ret = %u", rc);

	/* There will be two references on the handle here, once from this
	 * function, but a second one from iof_bulk_write_handler() itself
	 */
	if (handle)
		ios_fh_decref(handle, 2);

	return 0;
}

static void
iof_write_bulk_handler(crt_rpc_t *rpc)
{
	struct iof_write_bulk *in = crt_req_get(rpc);
	struct iof_write_out *out = crt_reply_get(rpc);
	struct ionss_file_handle *handle;
	struct crt_bulk_desc bulk_desc = {0};
	crt_bulk_t local_bulk_hdl = {0};
	d_sg_list_t sgl = {0};
	d_iov_t iov = {0};
	d_size_t len;

	int rc;

	VALIDATE_ARGS_GAH_FILE(rpc, in, out, handle);
	if (out->err)
		goto out;

	VALIDATE_WRITE(handle->projection, out);
	if (out->err || out->rc)
		goto out;

	rc = crt_req_addref(rpc);
	if (rc) {
		out->err = IOF_ERR_CART;
		goto out;
	}

	rc = crt_bulk_get_len(in->bulk, &len);
	if (rc || len == 0) {
		out->err = IOF_ERR_CART;
		goto out_decref;
	}

	rc = posix_memalign(&iov.iov_buf, 4096, len);
	if (rc || !iov.iov_buf) {
		out->err = IOF_ERR_NOMEM;
		goto out_decref;
	}
	iov.iov_len = len;
	iov.iov_buf_len = len;
	sgl.sg_iovs = &iov;
	sgl.sg_nr.num = 1;

	rc = crt_bulk_create(rpc->cr_ctx, &sgl, CRT_BULK_RW, &local_bulk_hdl);
	if (rc) {
		free(iov.iov_buf);
		out->err = IOF_ERR_CART;
		goto out_decref;
	}

	bulk_desc.bd_rpc = rpc;
	bulk_desc.bd_bulk_op = CRT_BULK_GET;
	bulk_desc.bd_remote_hdl = in->bulk;
	bulk_desc.bd_local_hdl = local_bulk_hdl;
	bulk_desc.bd_len = len;

	rc = crt_bulk_transfer(&bulk_desc, iof_write_bulk, NULL, NULL);
	if (rc) {
		free(iov.iov_buf);
		out->err = IOF_ERR_CART;
		goto out_decref;
	}

	/* Do not call crt_reply_send() in this case as it'll be done in
	 * the bulk handler.
	 */
	return;

out_decref:
	rc = crt_req_decref(rpc);

	if (rc)
		IOF_LOG_ERROR("Unable to drop reference, ret = %u", rc);
out:
	rc = crt_reply_send(rpc);

	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	if (handle)
		ios_fh_decref(handle, 1);
}

static void
iof_utimens_handler(crt_rpc_t *rpc)
{
	struct iof_time_in *in = crt_req_get(rpc);
	struct iof_status_out *out = crt_reply_get(rpc);
	int rc;

	VALIDATE_ARGS_STR(rpc, in, out);
	if (out->err)
		goto out;

	VALIDATE_WRITE(&base.fs_list[in->fs_id], out);
	if (out->err || out->rc)
		goto out;

	if (!in->time.iov_buf) {
		IOF_LOG_ERROR("No input times");
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_INFO("Setting times for %s", in->path);

	errno = 0;
	rc = utimensat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->path),
		       in->time.iov_buf, AT_SYMLINK_NOFOLLOW);

	if (rc)
		out->rc = errno;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);
}

static void iof_utimens_gah_handler(crt_rpc_t *rpc)
{
	struct iof_time_gah_in *in = crt_req_get(rpc);
	struct iof_status_out *out = crt_reply_get(rpc);
	struct ionss_file_handle *handle;
	int rc;

	VALIDATE_ARGS_GAH_FILE(rpc, in, out, handle);
	if (out->err)
		goto out;

	VALIDATE_WRITE(handle->projection, out);
	if (out->err || out->rc)
		goto out;

	if (!in->time.iov_buf) {
		IOF_LOG_ERROR("No input times");
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(in->gah));

	errno = 0;
	rc = futimens(handle->fd, in->time.iov_buf);

	if (rc)
		out->rc = errno;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	if (handle)
		ios_fh_decref(handle, 1);
}

static void iof_statfs_handler(crt_rpc_t *rpc)
{
	struct iof_gah_in *in = crt_req_get(rpc);
	struct iof_data_out *out = crt_reply_get(rpc);
	struct ionss_file_handle *handle;
	struct statvfs buf;
	int rc;

	VALIDATE_ARGS_GAH_FILE(rpc, in, out, handle);
	if (out->err)
		goto out;

	errno = 0;
	rc = fstatvfs(handle->fd, &buf);

	if (rc) {
		out->rc = errno;
		goto out;
	}

	/* Fuse ignores these three values on the client so zero them
	 * out here first
	 */
	buf.f_favail = 0;
	buf.f_fsid = 0;
	buf.f_flag = 0;
	d_iov_set(&out->data, &buf, sizeof(buf));

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	if (handle)
		ios_fh_decref(handle, 1);
}

static void iof_register_default_handlers(void)
{
#define DECL_RPC_HANDLER(NAME, FN) [DEF_RPC_TYPE(DEFAULT, NAME)] = FN

	crt_rpc_cb_t handlers[] = {
		DECL_RPC_HANDLER(opendir, iof_opendir_handler),
		DECL_RPC_HANDLER(readdir, iof_readdir_handler),
		DECL_RPC_HANDLER(closedir, iof_closedir_handler),
		DECL_RPC_HANDLER(getattr, iof_getattr_handler),
		DECL_RPC_HANDLER(getattr_gah, iof_getattr_gah_handler),
		DECL_RPC_HANDLER(write_direct, iof_write_direct_handler),
		DECL_RPC_HANDLER(write_bulk, iof_write_bulk_handler),
		DECL_RPC_HANDLER(truncate, iof_truncate_handler),
		DECL_RPC_HANDLER(ftruncate, iof_ftruncate_handler),
		DECL_RPC_HANDLER(chmod, iof_chmod_handler),
		DECL_RPC_HANDLER(chmod_gah, iof_chmod_gah_handler),
		DECL_RPC_HANDLER(rmdir, iof_rmdir_handler),
		DECL_RPC_HANDLER(rename, iof_rename_handler),
		DECL_RPC_HANDLER(read_bulk, iof_read_bulk_handler),
		DECL_RPC_HANDLER(unlink, iof_unlink_handler),
		DECL_RPC_HANDLER(open, iof_open_handler),
		DECL_RPC_HANDLER(create, iof_create_handler),
		DECL_RPC_HANDLER(read, iof_read_handler),
		DECL_RPC_HANDLER(close, iof_close_handler),
		DECL_RPC_HANDLER(mkdir, iof_mkdir_handler),
		DECL_RPC_HANDLER(readlink, iof_readlink_handler),
		DECL_RPC_HANDLER(symlink, iof_symlink_handler),
		DECL_RPC_HANDLER(fsync, iof_fsync_handler),
		DECL_RPC_HANDLER(fdatasync, iof_fdatasync_handler),
		DECL_RPC_HANDLER(utimens, iof_utimens_handler),
		DECL_RPC_HANDLER(utimens_gah, iof_utimens_gah_handler),
		DECL_RPC_HANDLER(statfs, iof_statfs_handler),
	};
	iof_register(DEF_PROTO_CLASS(DEFAULT), handlers);
}

/*
 * Process filesystem query from CNSS
 * This function currently uses dummy data to send back to CNSS
 */
static void
iof_query_handler(crt_rpc_t *query_rpc)
{
	struct iof_psr_query *query = crt_reply_get(query_rpc);
	int ret;

	query->max_read = base.max_read;
	query->max_write = base.max_write;
	query->readdir_size = base.max_readdir;

	d_iov_set(&query->query_list, base.fs_list,
		  base.projection_count * sizeof(struct iof_fs_info));

	ret = crt_reply_send(query_rpc);
	if (ret)
		IOF_LOG_ERROR("query rpc response not sent, ret = %d", ret);

	cnss_count++;
}

int ionss_register(void)
{
	int i, reg_count, ret;

	ret = crt_rpc_srv_register(QUERY_PSR_OP, &QUERY_RPC_FMT,
			iof_query_handler);
	if (ret) {
		IOF_LOG_ERROR("Cannot register query RPC, ret = %d", ret);
		return ret;
	}

	ret = crt_rpc_srv_register(DETACH_OP, NULL, cnss_detach_handler);
	if (ret) {
		IOF_LOG_ERROR("Cannot register CNSS detach"
				" RPC, ret = %d", ret);
		return ret;
	}

	ret = crt_rpc_srv_register(SHUTDOWN_BCAST_OP,
				   NULL, shutdown_handler);
	if (ret) {
		IOF_LOG_ERROR("Cannot register shutdown "
				"broadcast RPC handler, ret = %d", ret);
		return ret;
	}
	ret = crt_rpc_register(SHUTDOWN_BCAST_OP, NULL);
	if (ret) {
		IOF_LOG_ERROR("Cannot register shutdown "
				"broadcast RPC, ret = %d", ret);
		return ret;
	}
	reg_count = sizeof(register_handlers)
		  / sizeof(*register_handlers);
	for (i = 0; i < reg_count; i++)
		register_handlers[i]();
	return ret;
}

static int check_shutdown(void *arg)
{
	int *valuep = (int *)arg;
	return *valuep;
}

static void *progress_thread(void *arg)
{
	int			rc;
	struct ios_base *b = (struct ios_base *)arg;

	/* progress loop */
	do {
		rc = crt_progress(b->crt_ctx, b->poll_interval,
				  check_shutdown, &shutdown);
		if (rc != 0 && rc != -CER_TIMEDOUT) {
			IOF_LOG_ERROR("crt_progress failed rc: %d", rc);
			break;
		}

	} while (!shutdown);

	IOF_LOG_DEBUG("progress_thread exiting");

	pthread_exit(NULL);
}

/* Find an entry in the hash table.
 *
 * As chash_table_traverse() does not support removal from the callback function
 * save a pointer in *arg and return 1 to terminate the traverse.  This way we
 * can iterate over the entries in the hash table and delete every one.
 */
static int ioc_ht_find(d_list_t *rlink, void *arg)
{
	d_list_t **p = arg;

	*p = rlink;
	return 1;
}

/* Return the first entry in a hash table.  Do this by traversing the table, and
 * returning the first rlink value provided to the callback.
 * Returns rlink on success, or NULL on error or if the hash table is empty.
 */
static d_list_t *chash_table_first(struct chash_table *ht)
{
	d_list_t *rlink = NULL;
	int rc;

	rc = chash_table_traverse(ht, ioc_ht_find, &rlink);
	if (rc < 0)
		return NULL;

	return rlink;
}

/* Close all file handles associated with a projection, and release all GAH
 * which are currently in use.
 */
static void release_projection_resources(struct ios_projection *projection)
{
	struct ionss_file_handle *fh;
	int rc;

	do {
		d_list_t *rlink;

		fh = NULL;

		rlink = chash_table_first(&projection->file_ht);
		if (rlink) {
			fh = container_of(rlink, struct ionss_file_handle,
					  clist);
			ios_fh_decref(fh, fh->ref);
		}
	} while (fh);

	rc = chash_table_destroy_inplace(&projection->file_ht, false);
	if (rc != 0)
		IOF_LOG_ERROR("Failed to destroy hash table");
}

int fslookup_entry(struct mntent *entry, void *priv)
{
	int *path_lengths = priv;
	int i, cur_path_len;

	cur_path_len = strnlen(entry->mnt_dir, IOF_MAX_MNTENT_LEN);
	for (i = 0; i < base.projection_count; i++) {
		if (strstr(base.projection_array[i].full_path,
			   entry->mnt_dir) == NULL)
			continue;
		if (cur_path_len < path_lengths[i])
			continue;
		path_lengths[i] = cur_path_len;
		strncpy(base.projection_array[i].fs_type,
			entry->mnt_type, IOF_MAX_FSTYPE_LEN);

		/* Safety Check */
		if (base.projection_array[i].fs_type
		    [IOF_MAX_FSTYPE_LEN - 1] != '\0') {
			IOF_LOG_ERROR("Overflow parsing File System"
					" type: %s", entry->mnt_type);
			return -ERANGE;
		}
	}
	return 0;
}

/*
 * Identify the type of file system for projected paths using the longest
 * matching prefix to determine the mount point. This is used to turn
 * specific features on or off depending on the type of file system.
 * e.g. Distributed Metadata for Parallel File Systems.
 *
 */
int filesystem_lookup(void)
{
	int i, rc = 0, *path_lengths;

	errno = 0;
	path_lengths = calloc(sizeof(*path_lengths),
			      base.projection_count);
	if (path_lengths == NULL) {
		IOF_LOG_ERROR("Could not allocate memory [path_lengths]");
		return -ENOMEM;
	}

	rc = iof_mntent_foreach(fslookup_entry, path_lengths);
	if (rc) {
		IOF_LOG_ERROR("Error parsing mount entries.");
		goto cleanup;
	}

	for (i = 0; i < base.projection_count; i++) {
		if (path_lengths[i] == 0) {
			IOF_LOG_ERROR("No mount point found for path %s",
				      base.projection_array[i].full_path);
			rc = -ENOENT;
			continue;
		}
		IOF_LOG_DEBUG("File System: %s; Path: %s",
			      base.projection_array[i].fs_type,
			      base.projection_array[i].full_path);
	}

cleanup:
	if (path_lengths)
		free(path_lengths);
	return rc;
}

/*
 * Parse a uint32_t from a command line option and allow either k or m
 * suffixes.  Updates the value if str contains a valid value or returns
 * -1 on failure.
 */
static int parse_size(uint32_t *value, const char *str)
{
	uint32_t new_value = *value;
	int ret;

	/* Read the first size */
	ret = sscanf(str, "%u", &new_value);
	if (ret != 1)
		return -1;

	/* Advance pch to the next non-numeric character */
	while (isdigit(*str))
		str++;

	if (*str == 'k')
		new_value *= 1024;
	else if (*str == 'm')
		new_value *= (1024 * 1024);
	else if (*str != '\0')
		return -1;

	*value = new_value;
	return 0;
}

void show_help(const char *prog)
{
	printf("I/O Forwarding I/O Node System Services\n");
	printf("\n");
	printf("Usage: %s [OPTION]... [PATH]...\n", prog);
	printf("Projects filesystem access to PATHs from remote I/O Forwarding instances\n");
	printf("\n");
	printf("\t\t--group-name\tName of CaRT group to form\n");
	printf("\t\t--poll-interval\tCaRT Poll interval to use on IONSS\n");
	printf("\t\t--max-read\tMaximum size of read requests\n");
	printf("\t\t--max-write\tMaximum size of write requests\n");
	printf("\t-h\t--help\t\tThis help text\n");
	printf("\n");
}

static int
fh_init(void *arg, void *handle)
{
	struct ionss_file_handle *fh = arg;

	fh->projection = handle;
	return 0;
}

static int
ar_init(void *arg, void *handle)
{
	struct ionss_active_read *ard = arg;
	struct ios_projection *projection = handle;
	int rc;

	ard->projection = projection;

	ard->buf_len = projection->base->max_read;
	rc = posix_memalign(&ard->buf, 4096, ard->buf_len);
	if (rc || !ard->buf)
		return -1;

	return 0;
}

static int
ar_clean(void *arg)
{
	struct ionss_active_read *ard = arg;
	d_sg_list_t sgl = {0};
	d_iov_t iov = {0};
	int rc;

	if (ard->local_bulk_handle)
		crt_bulk_free(ard->local_bulk_handle);

	iov.iov_len = ard->buf_len;
	iov.iov_buf = ard->buf;
	iov.iov_buf_len = ard->buf_len;
	sgl.sg_iovs = &iov;
	sgl.sg_nr.num = 1;

	rc = crt_bulk_create(ard->projection->base->crt_ctx, &sgl, CRT_BULK_RO,
			     &ard->local_bulk_handle);
	if (rc) {
		free(ard->buf);
		return -1;
	}
	return 0;
}

static void
ar_release(void *arg)
{
	struct ionss_active_read *ard = arg;
	int rc;

	rc = crt_bulk_free(ard->local_bulk_handle);

	IOF_LOG_DEBUG("Bulk free %p %d", ard, rc);

	free(ard->buf);
}

int main(int argc, char **argv)
{
	char *ionss_grp = IOF_DEFAULT_SET;
	int i;
	int ret;
	unsigned int thread_count = 2;
	int err;
	int c;
	int cnss_threads = 0;

	char *version = iof_get_version();

	iof_log_init("ION", "IONSS", NULL);
	IOF_LOG_INFO("IONSS version: %s", version);

	base.poll_interval = 1000 * 1000;
	base.max_read = 1024 * 1024;
	base.max_write = 1024 * 1024;
	base.max_iov_read = 64;
	base.max_readdir = 1024 * 64;

	while (1) {
		static struct option long_options[] = {
			{"group-name", required_argument, 0, 1},
			{"poll-interval", required_argument, 0, 2},
			{"max-read", required_argument, 0, 3},
			{"max-write", required_argument, 0, 4},
			{"readdir-size", required_argument, 0, 5},
			{"max-direct-read", required_argument, 0, 6},
			{"cnss-threads", no_argument, 0, 7},
			{"thread-count", required_argument, 0, 't'},
			{"help", no_argument, 0, 'h'},
			{0, 0, 0, 0}
		};

		c = getopt_long(argc, argv, "h", long_options, NULL);

		if (c == -1)
			break;

		switch (c) {
		case 1:
			ionss_grp = optarg;
			break;
		case 2:
			ret = sscanf(optarg, "%u", &base.poll_interval);
			if (ret != 1) {
				printf("Unable to set poll interval to %s\n",
				       optarg);
			}
			break;
		case 3:
			ret = parse_size(&base.max_read, optarg);
			if (ret != 0) {
				printf("Unable to set max read to %s\n",
				       optarg);
			}
			break;
		case 4:
			ret = parse_size(&base.max_write, optarg);
			if (ret != 0) {
				printf("Unable to set max write to %s\n",
				       optarg);
			}
			break;
		case 5:
			ret = parse_size(&base.max_readdir, optarg);
			if (ret != 0) {
				printf("Unable to set readdir size to %s\n",
				       optarg);
			}
			break;
		case 6:
			ret = parse_size(&base.max_iov_read, optarg);
			if (ret != 0) {
				printf("Unable to set read-direct size to %s\n",
				       optarg);
			}
			break;
		case 7:
			cnss_threads = 1;
			break;
		case 't':
			ret = sscanf(optarg, "%d", &thread_count);
			if (ret != 1 || thread_count < 1) {
				printf("Unable to set thread count to %s\n",
				       optarg);
			}
			break;

		case 'h':
			show_help(argv[0]);
			exit(0);
			break;
		case '?':
			exit(1);
			break;
		}
	}

	base.projection_count = argc - optind;

	if (base.projection_count < 1) {
		IOF_LOG_ERROR("Expected at least one directory as command line option");
		return IOF_BAD_DATA;
	}

	/*hardcoding the number and path for projected filesystems*/
	base.fs_list = calloc(base.projection_count,
			      sizeof(struct iof_fs_info));
	if (!base.fs_list) {
		IOF_LOG_ERROR("Filesystem list not allocated");
		ret = IOF_ERR_NOMEM;
		goto cleanup;
	}

	base.projection_array = calloc(base.projection_count,
				       sizeof(*base.projection_array));
	if (!base.projection_array) {
		IOF_LOG_ERROR("Failed to allocate memory");
		ret = IOF_ERR_NOMEM;
		goto cleanup;
	}

	ret = iof_pool_init(&base.pool);
	if (ret != 0) {
		ret = IOF_ERR_NOMEM;
		goto cleanup;
	}

	IOF_LOG_INFO("Projecting %d exports", base.projection_count);

	base.gs = ios_gah_init();

	/*
	 * Populate the projection_array with every projection.
	 *
	 * Exports must be directories.
	 * Exports are identified by the absolute path, without allowing for
	 * symbolic links.
	 * The maximum path length of exports is checked.
	 */
	err = 0;

	for (i = 0; i < base.projection_count; i++) {
		struct ios_projection *projection = &base.projection_array[i];
		struct stat buf = {0};
		char *proj_path = argv[i + optind];
		struct iof_pool_reg fhp = { .handle = projection,
					    .init = fh_init,
					    POOL_TYPE_INIT(ionss_file_handle,
							   clist)};
		int fd;
		int rc;

		fd = open(proj_path,
			  O_DIRECTORY | O_PATH | O_NOATIME | O_RDONLY);
		if (fd == -1) {
			IOF_LOG_ERROR("Could not open export directory %s",
				      proj_path);
			err = 1;

			continue;
		}

		projection->active = 0;
		projection->base = &base;
		rc = chash_table_create_inplace(D_HASH_FT_RWLOCK, 5, NULL,
						&hops, &projection->file_ht);
		if (rc != 0) {
			IOF_LOG_ERROR("Could not create hash table");
			continue;
		}
		pthread_mutex_init(&projection->lock, NULL);

		projection->max_read_count = 3;
		D_INIT_LIST_HEAD(&projection->read_list);

		rc = fstat(fd, &buf);
		if (rc) {
			IOF_LOG_ERROR("Could not stat export path %s %d",
				      proj_path, errno);
			err = 1;
			continue;
		}

		projection->dev_no = buf.st_dev;

		/* Set feature flags. These will be sent to the client */
		projection->flags = IOF_FS_DEFAULT;
		if (faccessat(fd, ".", W_OK, 0) == 0)
			projection->flags |= IOF_WRITEABLE;

		projection->fh_pool = iof_pool_register(&base.pool, &fhp);
		if (!projection->fh_pool)
			continue;

		rc = ios_fh_alloc(projection, &projection->root);
		if (rc != 0)
			continue;

		projection->root->fd = fd;
		projection->root->mf.inode_no = buf.st_ino;
		chash_rec_insert(&projection->file_ht, &projection->root->mf,
				  sizeof(projection->root->mf),
				  &projection->root->clist, 0);

		if (cnss_threads)
			projection->flags |= IOF_CNSS_MT;

		IOF_LOG_INFO("Projecting %s %#x", proj_path,
			     (int)projection->flags);

		projection->active = 1;
		projection->full_path = strdup(proj_path);
		projection->id = i;
	}
	if (err) {
		ret = 1;
		goto cleanup;
	}

	ret = filesystem_lookup();
	if (ret) {
		IOF_LOG_ERROR("File System look up failed with ret = %d", ret);
		goto cleanup;
	}

	/*initialize CaRT*/
	ret = crt_init(ionss_grp, CRT_FLAG_BIT_SERVER);
	if (ret) {
		IOF_LOG_ERROR("Crt_init failed with ret = %d", ret);
		goto cleanup;
	}

	cnss_count = 0;
	base.primary_group = crt_group_lookup(ionss_grp);
	if (base.primary_group == NULL) {
		IOF_LOG_ERROR("Failed to look up primary group");
		ret = 1;
		goto cleanup;
	}
	IOF_LOG_INFO("Primary Group: %s", base.primary_group->cg_grpid);
	crt_group_rank(base.primary_group, &base.my_rank);
	crt_group_size(base.primary_group, &base.num_ranks);

	ret = crt_context_create(NULL, &base.crt_ctx);
	if (ret) {
		IOF_LOG_ERROR("Could not create context");
		goto cleanup;
	}

	for (i = 0; i < base.projection_count; i++) {
		struct ios_projection *projection = &base.projection_array[i];
		struct iof_pool_reg arp = {.handle = projection,
					   .init = ar_init,
					   .clean = ar_clean,
					   .release = ar_release,
					   .max_desc = 3,
					   POOL_TYPE_INIT(ionss_active_read,
							  list)};

		if (!projection->active)
			continue;
		projection->ar_pool = iof_pool_register(&base.pool, &arp);
		if (!projection->ar_pool)
			projection->active = 0;
	}

	/* Create a fs_list from the projection array */
	for (i = 0; i < base.projection_count ; i++) {
		struct ios_projection *projection = &base.projection_array[i];

		if (!projection->active) {
			IOF_LOG_WARNING("Not projecting %s",
					projection->full_path);
			continue;
		}
		base.fs_list[i].flags = projection->flags;
		base.fs_list[i].gah = projection->root->gah;
		base.fs_list[i].id = projection->id;
		strncpy(base.fs_list[i].mnt, projection->full_path,
			IOF_NAME_LEN_MAX);
	}

	ionss_register();

	shutdown = 0;

	if (thread_count == 1) {
		int rc;
		/* progress loop */
		do {
			rc = crt_progress(base.crt_ctx, base.poll_interval,
					  check_shutdown, &shutdown);
			if (rc != 0 && rc != -CER_TIMEDOUT) {
				IOF_LOG_ERROR("crt_progress failed rc: %d", rc);
				break;
			}
		} while (!shutdown);

	} else {
		pthread_t *progress_tids;
		int thread;

		progress_tids = calloc(thread_count, sizeof(*progress_tids));
		if (!progress_tids) {
			ret = 1;
			goto cleanup;
		}
		for (thread = 0; thread < thread_count ; thread++) {
			IOF_LOG_INFO("Starting thread %d", thread);
			ret = pthread_create(&progress_tids[thread], NULL,
					     progress_thread, &base);
		}

		for (thread = 0; thread < thread_count ; thread++) {
			ret = pthread_join(progress_tids[thread], NULL);

			if (ret)
				IOF_LOG_ERROR("Could not join progress thread %d",
					      thread);
		}
		free(progress_tids);
	}

	IOF_LOG_INFO("Shutting down, threads terminated");

	/* After shutdown has been invoked close all files and free any memory,
	 * in normal operation all files should be closed as a result of CNSS
	 * requests prior to shutdown being triggered however perform a full
	 * shutdown here and log any which remained open.
	 */
	for (i = 0; i < base.projection_count; i++) {
		struct ios_projection *projection = &base.projection_array[i];

		if (!projection->active)
			continue;

		/* Close all file handles associated with a projection.
		 *
		 * No locks are held here because at this point all progression
		 * threads have already been terminated
		 */
		release_projection_resources(projection);

		pthread_mutex_destroy(&projection->lock);

		free(projection->full_path);
	}

	iof_pool_destroy(&base.pool);

	ret = crt_context_destroy(base.crt_ctx, 0);
	if (ret)
		IOF_LOG_ERROR("Could not destroy context");

	free(base.projection_array);

cleanup:
	/* TODO:
	 *
	 * This means a resource leak, or failed cleanup after client eviction.
	 * We really should have the ability to iterate over any handles that
	 * remain open at this point.
	 */

	ret = crt_finalize();
	if (ret)
		IOF_LOG_ERROR("Could not finalize cart");

	if (base.fs_list)
		free(base.fs_list);

	ret = ios_gah_destroy(base.gs);
	if (ret)
		IOF_LOG_ERROR("Could not close GAH pool");

	/* Memset base to zero to delete any dangling memory references so that
	 * valgrind can better detect lost memory
	 */
	memset(&base, 0, sizeof(base));

	iof_log_close();

	return ret;
}
