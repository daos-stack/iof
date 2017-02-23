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
#include <dirent.h>

#include <sys/types.h>
#include <sys/stat.h>

#ifdef __APPLE__
#include <sys/syslimits.h>
#endif

#include <fcntl.h>

#include "version.h"
#include "log.h"
#include "iof_common.h"
#include "ios_gah.h"

#define IOF_MAX_PATH_LEN 4096

static struct iof_fs_info *fs_list;
static struct ios_gah_store *gs;
static struct ionss_projection *projections;
static int num_fs;
static int shutdown;

#define IONSS_READDIR_ENTRIES_PER_RPC (2)

struct ionss_projection {
	DIR	*dir;
	int	dir_fd;
};

struct ionss_dir_handle {
	int	fs_id;
	char	*h_name;
	DIR	*h_dir;
	int	fd;
};

struct ionss_file_handle {
	int	fs_id;
	int	fd;
	ino_t	inode_no;
};

struct proto *proto;

int shutdown_handler(crt_rpc_t *rpc)
{
	int rc;

	IOF_LOG_DEBUG("Shutdown request received");
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, rc = %u", rc);

	shutdown = 1;

	return 0;
}

#define ID_TO_FD(ID) (projections[(ID)].dir_fd)

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
	if (id >= num_fs) {
		IOF_LOG_ERROR("Filesystem ID invalid");
		return IOF_BAD_DATA;
	}

	mnt = fs_list[id].mnt;

	ret = snprintf(new_path, IOF_MAX_PATH_LEN, "%s%s", mnt,
			old_path);
	if (ret > IOF_MAX_PATH_LEN)
		return IOF_ERR_OVERFLOW;
	IOF_LOG_DEBUG("New Path: %s", new_path);

	return IOF_SUCCESS;
}

int iof_getattr_handler(crt_rpc_t *rpc)
{
	struct iof_string_in *in;
	struct iof_getattr_out *out;
	struct stat stbuf = {0};
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out_no_log;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (!in->path) {
		IOF_LOG_ERROR("No input path");
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_DEBUG("path %d %s", in->fs_id, in->path);

	if (in->fs_id >= num_fs) {
		out->err = IOF_BAD_DATA;
		goto out;
	}

	errno = 0;
	rc = fstatat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->path), &stbuf,
		     AT_SYMLINK_NOFOLLOW);
	if (rc)
		out->rc = errno;
	else
		crt_iov_set(&out->stat, &stbuf, sizeof(struct stat));

out:

	IOF_LOG_DEBUG("path %s result err %d rc %d",
		      in->path, out->err, out->rc);

out_no_log:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, rc = %u", rc);

	return 0;
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
int iof_getattr_gah_handler(crt_rpc_t *rpc)
{
	struct ionss_file_handle *handle = NULL;
	struct iof_gah_in *in;
	struct iof_getattr_out *out;
	struct stat stbuf = {0};
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(in->gah));

	rc = ios_gah_get_info(gs, &in->gah, (void **)&handle);
	if (rc != IOS_SUCCESS || !handle) {
		out->err = IOF_GAH_INVALID;
		IOF_LOG_DEBUG("Failed to load fd from gah %p %d",
			      &in->gah, rc);
		goto out;
	}

	errno = 0;
	rc = fstat(handle->fd, &stbuf);

	/* Cache the inode number */
	handle->inode_no = stbuf.st_ino;

	if (rc)
		out->rc = errno;
	else
		crt_iov_set(&out->stat, &stbuf, sizeof(struct stat));

out:
	IOF_LOG_DEBUG("result err %d rc %d",
		      out->err, out->rc);

	rc = crt_reply_send(rpc);

	if (rc)
		IOF_LOG_ERROR("response not sent, rc = %u", rc);

	return 0;
}

int iof_opendir_handler(crt_rpc_t *rpc)
{
	struct iof_string_in *in;
	struct iof_opendir_out *out;
	struct ionss_dir_handle *local_handle;
	int rc;
	int fd;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out_no_log;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out_no_log;
	}

	if (!in->path) {
		IOF_LOG_ERROR("No input path");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (in->fs_id >= num_fs) {
		out->err = IOF_BAD_DATA;
		goto out;
	}

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
	local_handle->h_name = strdup(in->path);
	local_handle->fs_id = in->fs_id;

	rc = ios_gah_allocate(gs, &out->gah, 0, 0, local_handle);
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

out_no_log:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, rc = %u", rc);
	return 0;
}

/*
 * Read dirent from a directory and reply to the origin.
 *
 * TODO:
 * Handle offset properly.  Currently offset is passed as part of the RPC
 * but not dealt with at all.
 * Use readdir_r().  This code is not thread safe.
 * Parse GAH better.  If a invalid GAH is passed then it's handled but we
 * really should pass this back to the client properly so it doesn't retry.
 *
 */
int iof_readdir_handler(crt_rpc_t *rpc)
{
	struct iof_readdir_in *in;
	struct iof_readdir_out *out;
	struct ionss_dir_handle *handle = NULL;
	struct dirent *dir_entry;
	struct iof_readdir_reply replies[IONSS_READDIR_ENTRIES_PER_RPC] = {0};
	int reply_idx = 0;
	int rc;

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		return 0;
	}

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		return 0;
	}

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(in->gah));

	rc = ios_gah_get_info(gs, &in->gah, (void **)&handle);
	if (rc != IOS_SUCCESS || !handle) {
		out->err = IOF_GAH_INVALID;
		IOF_LOG_DEBUG("Failed to load handle from gah %p %d",
			      &in->gah, rc);
		goto out;
	}

	IOF_LOG_DEBUG("Reading %p", handle->h_dir);
	reply_idx = 0;
	do {
		errno = 0;
		dir_entry = readdir(handle->h_dir);

		if (!dir_entry) {
			if (errno == 0) {
				/* End of directory */
				replies[reply_idx].read_rc = 0;
				replies[reply_idx].last = 1;
			} else {
				/* An error occoured */
				replies[reply_idx].read_rc = errno;
			}
			reply_idx++;
			goto out;
		}

		if (strncmp(".", dir_entry->d_name, 2) == 0)
			continue;

		if (strncmp("..", dir_entry->d_name, 3) == 0)
			continue;

		/* TODO: Check this */
		strncpy(replies[reply_idx].d_name, dir_entry->d_name, NAME_MAX);

		errno = 0;
		rc = fstatat(handle->fd,
			     replies[reply_idx].d_name,
			     &replies[reply_idx].stat,
			     AT_SYMLINK_NOFOLLOW);
		if (rc != 0)
			replies[reply_idx].stat_rc = errno;

		reply_idx++;
	} while (reply_idx < (IONSS_READDIR_ENTRIES_PER_RPC));

out:

	IOF_LOG_INFO("Sending %d replies", reply_idx);

	if (reply_idx)
		crt_iov_set(&out->replies, &replies[0],
			    sizeof(struct iof_readdir_reply) * reply_idx);

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR(" response not sent, rc = %u", rc);
	return 0;
}

int iof_closedir_handler(crt_rpc_t *rpc)
{
	struct iof_gah_in *in = NULL;
	struct ionss_dir_handle *handle = NULL;
	int rc;

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		return 0;
	}

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(in->gah));

	rc = ios_gah_get_info(gs, &in->gah, (void **)&handle);
	if (rc != IOS_SUCCESS)
		IOF_LOG_DEBUG("Failed to load DIR* from gah %p %d",
			      &in->gah, rc);

	if (handle) {
		IOF_LOG_DEBUG("Closing %p", handle->h_dir);
		rc = closedir(handle->h_dir);
		if (rc != 0)
			IOF_LOG_DEBUG("Failed to close directory %p",
				      handle->h_dir);
		free(handle->h_name);
		free(handle);
	}

	ios_gah_deallocate(gs, &in->gah);

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, rc = %u", rc);
	return 0;
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

int iof_open_handler(crt_rpc_t *rpc)
{
	struct iof_open_in *in;
	struct iof_open_out *out;
	struct ionss_file_handle *local_handle = NULL;
	struct ios_gah gah = {0};
	int fd;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out_no_log;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out_no_log;
	}

	if (!in->path) {
		IOF_LOG_ERROR("No input path");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (in->fs_id >= num_fs) {
		out->err = IOF_BAD_DATA;
		goto out;
	}

	IOF_LOG_DEBUG("path %s flags 0%o",
		      in->path, in->flags);

	errno = 0;
	fd = openat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->path), in->flags);
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
	local_handle->fs_id = in->fs_id;

	rc = ios_gah_allocate(gs, &gah, 0, 0, local_handle);
	if (rc != IOS_SUCCESS) {
		close(fd);
		free(local_handle);
		out->err = IOF_ERR_INTERNAL;
		goto out;
	}

	IOF_LOG_INFO("Handle %p " GAH_PRINT_FULL_STR, local_handle,
		     GAH_PRINT_FULL_VAL(gah));

	out->gah = gah;

out:
	IOF_LOG_DEBUG("path %s flags 0%o ", in->path, in->flags);

	LOG_FLAGS(local_handle, in->flags);

	IOF_LOG_INFO("path %s result err %d rc %d",
		     in->path, out->err, out->rc);

out_no_log:

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);
	return 0;
}

int iof_create_handler(crt_rpc_t *rpc)
{
	struct iof_create_in *in;
	struct iof_open_out *out;
	struct ionss_file_handle *local_handle;
	struct ios_gah gah = {0};
	int fd;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out_no_log;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out_no_log;
	}

	if (!in->path) {
		IOF_LOG_ERROR("No input path");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (in->fs_id >= num_fs) {
		out->err = IOF_BAD_DATA;
		goto out;
	}

	IOF_LOG_DEBUG("path %s flags 0%o mode 0%o",
		      in->path, in->flags, in->mode);

	errno = 0;
	fd = openat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->path), in->flags,
		    in->mode);
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
	local_handle->fs_id = in->fs_id;

	rc = ios_gah_allocate(gs, &gah, 0, 0, local_handle);
	if (rc != IOS_SUCCESS) {
		close(fd);
		free(local_handle);
		out->err = IOF_ERR_INTERNAL;
		goto out;
	}

	IOF_LOG_INFO("Handle %p " GAH_PRINT_FULL_STR, local_handle,
		     GAH_PRINT_FULL_VAL(gah));

	out->gah = gah;

out:
	IOF_LOG_DEBUG("path %s flags 0%o mode 0%o 0%o", in->path, in->flags,
		      in->mode & S_IFREG, in->mode & ~S_IFREG);

	LOG_FLAGS(local_handle, in->flags);
	LOG_MODES(local_handle, in->mode);

	IOF_LOG_INFO("path %s result err %d rc %d",
		     in->path, out->err, out->rc);

out_no_log:

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);
	return 0;
}

int iof_close_handler(crt_rpc_t *rpc)
{
	struct iof_gah_in *in;
	struct ionss_file_handle *local_handle = NULL;
	int rc;

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		goto out;
	}

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(in->gah));

	rc = ios_gah_get_info(gs, &in->gah, (void **)&local_handle);
	if (rc != IOS_SUCCESS || !local_handle) {
		IOF_LOG_INFO("Failed to load handle from gah %p %d",
			     &in->gah, rc);
		goto out;
	}

	IOF_LOG_DEBUG("Closing handle %p fd %d", local_handle,
		      local_handle->fd);

	rc = close(local_handle->fd);
	if (rc != 0)
		IOF_LOG_ERROR("Failed to close file %d", local_handle->fd);

	free(local_handle);

	rc = ios_gah_deallocate(gs, &in->gah);
	if (rc)
		IOF_LOG_ERROR("Failed to deallocate GAH");

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);
	return 0;
}

int iof_fsync_handler(crt_rpc_t *rpc)
{
	struct iof_gah_in *in;
	struct iof_status_out *out;
	struct ionss_file_handle *local_handle = NULL;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(in->gah));

	rc = ios_gah_get_info(gs, &in->gah, (void **)&local_handle);
	if (rc != IOS_SUCCESS || !local_handle) {
		IOF_LOG_INFO("Failed to load handle from gah %p %d",
			     &in->gah, rc);
		goto out;
	}

	errno = 0;
	rc = fsync(local_handle->fd);
	if (rc)
		out->rc = errno;

out:
	IOF_LOG_DEBUG("result err %d rc %d",
		      out->err, out->rc);

	rc = crt_reply_send(rpc);

	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);
	return 0;
}

int iof_fdatasync_handler(crt_rpc_t *rpc)
{
	struct iof_gah_in *in;
	struct iof_status_out *out;
	struct ionss_file_handle *local_handle = NULL;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(in->gah));

	rc = ios_gah_get_info(gs, &in->gah, (void **)&local_handle);
	if (rc != IOS_SUCCESS || !local_handle) {
		IOF_LOG_INFO("Failed to load handle from gah %p %d",
			     &in->gah, rc);
		goto out;
	}

	errno = 0;
	rc = fdatasync(local_handle->fd);
	if (rc)
		out->rc = errno;

out:
	IOF_LOG_DEBUG("result err %d rc %d",
		      out->err, out->rc);

	rc = crt_reply_send(rpc);

	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);
	return 0;
}

int iof_read_handler(crt_rpc_t *rpc)
{
	struct iof_read_in *in;
	struct iof_data_out *out;
	struct ionss_file_handle *handle = NULL;
	void *data = NULL;
	size_t bytes_read;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(in->gah));

	rc = ios_gah_get_info(gs, &in->gah, (void **)&handle);
	if (rc != IOS_SUCCESS || !handle) {
		out->err = IOF_GAH_INVALID;
		IOF_LOG_DEBUG("Failed to load fd from gah %p %d",
			      &in->gah, rc);
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
		crt_iov_set(&out->data, data, bytes_read);

out:
	rc = crt_reply_send(rpc);

	if (rc)
		IOF_LOG_ERROR("response not sent, rc = %u", rc);

	if (data)
		free(data);

	return 0;
}

int iof_read_bulk_cb(const struct crt_bulk_cb_info *cb_info)
{
	struct iof_read_bulk_in *in;
	struct iof_read_bulk_out *out;
	crt_iov_t iov = {0};
	crt_sg_list_t sgl = {0};
	int rc;

	out = crt_reply_get(cb_info->bci_bulk_desc->bd_rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	if (cb_info->bci_rc) {
		out->err = IOF_ERR_CART;
		goto out;
	}

	in = crt_req_get(cb_info->bci_bulk_desc->bd_rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
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

	free(iov.iov_buf);
	out->len = iov.iov_buf_len;

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
 * The target of a bulk_read RPC from a client, replies using bulk data.
 *
 * Checks the read size
 * Allocates memory
 * Does the read
 * Creates a bulk handle
 * Submits the bulk handle
 */
int iof_read_bulk_handler(crt_rpc_t *rpc)
{
	struct iof_read_bulk_in *in;
	struct iof_read_bulk_out *out;
	struct ionss_file_handle *handle = NULL;
	void *data = NULL;
	size_t bytes_read;
	struct crt_bulk_desc bulk_desc = {0};
	crt_bulk_t local_bulk_hdl = {0};
	crt_sg_list_t sgl = {0};
	crt_iov_t iov = {0};
	crt_size_t len;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(in->gah));

	rc = ios_gah_get_info(gs, &in->gah, (void **)&handle);
	if (rc != IOS_SUCCESS || !handle) {
		out->err = IOF_GAH_INVALID;
		IOF_LOG_DEBUG("Failed to load fd from gah %p %d",
			      &in->gah, rc);
		goto out;
	}

	IOF_LOG_DEBUG("Reading from %d", handle->fd);

	rc = crt_bulk_get_len(in->bulk, &len);
	if (rc || len == 0) {
		out->err = IOF_ERR_CART;
		goto out;
	}

	data = malloc(len);
	if (!data) {
		out->err = IOF_ERR_NOMEM;
		goto out;
	}

	errno = 0;
	bytes_read = pread(handle->fd, data, len, in->base);
	if (bytes_read == -1) {
		out->rc = errno;
		goto out;
	} else if (!bytes_read) {
		goto out;
	}

	rc = crt_req_addref(rpc);
	if (rc) {
		out->err = IOF_ERR_CART;
		goto out;
	}

	iov.iov_len = bytes_read;
	iov.iov_buf = data;
	iov.iov_buf_len = bytes_read;
	sgl.sg_iovs = &iov;
	sgl.sg_nr.num = 1;

	rc = crt_bulk_create(rpc->cr_ctx, &sgl, CRT_BULK_RO, &local_bulk_hdl);
	if (rc) {
		out->err = IOF_ERR_CART;
		goto out;
	}

	bulk_desc.bd_rpc = rpc;
	bulk_desc.bd_bulk_op = CRT_BULK_PUT;
	bulk_desc.bd_remote_hdl = in->bulk;
	bulk_desc.bd_local_hdl = local_bulk_hdl;
	bulk_desc.bd_len = bytes_read;

	rc = crt_bulk_transfer(&bulk_desc, iof_read_bulk_cb, NULL, NULL);
	if (rc) {
		out->err = IOF_ERR_CART;
		goto out;
	}

	/* Do not call crt_reply_send() in this case as it'll be done in
	 * the bulk handler.
	 */

	return 0;

out:
	rc = crt_reply_send(rpc);

	if (rc)
		IOF_LOG_ERROR("response not sent, rc = %u", rc);

	if (data)
		free(data);

	return 0;
}

int iof_rename_handler(crt_rpc_t *rpc)
{
	struct iof_two_string_in *in;
	struct iof_status_out *out;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (!in->dst || !in->src) {
		IOF_LOG_ERROR("Missing inputs");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (in->fs_id >= num_fs) {
		out->err = IOF_BAD_DATA;
		goto out;
	}

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

	return 0;
}

int iof_symlink_handler(crt_rpc_t *rpc)
{
	struct iof_two_string_in *in;
	struct iof_status_out *out;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out_no_log;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (!in->dst || !in->src) {
		IOF_LOG_ERROR("Missing inputs");
		out->err = IOF_ERR_CART;
		goto out;
	}

	rc = symlinkat(in->dst, ID_TO_FD(in->fs_id),
		       iof_get_rel_path(in->src));

	if (rc)
		out->rc = errno;

out:
	IOF_LOG_DEBUG("src %s dst %s result err %d rc %d",
		      in->src, in->dst, out->err, out->rc);

out_no_log:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	return 0;
}

int iof_mkdir_handler(crt_rpc_t *rpc)
{
	struct iof_create_in *in;
	struct iof_status_out *out;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_DEBUG("path %d %s", in->fs_id, in->path);

	if (in->fs_id >= num_fs) {
		out->err = IOF_BAD_DATA;
		goto out;
	}

	if (!in->path) {
		out->err = IOF_BAD_DATA;
		goto out;
	}

	errno = 0;
	rc = mkdirat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->path), in->mode);

	if (rc)
		out->rc = errno;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	return 0;
}

/* This function needs additional checks to handle longer links.
 *
 * There is a upper limit on the lenth FUSE has provided in the CNSS but this
 * function should ensure that it's not introducing further restricions.
 */
int iof_readlink_handler(crt_rpc_t *rpc)
{
	struct iof_string_in *in;
	struct iof_string_out *out;
	char reply[IOF_MAX_PATH_LEN] = {0};
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (!in->path) {
		IOF_LOG_ERROR("No input path");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (in->fs_id >= num_fs) {
		out->err = IOF_BAD_DATA;
		goto out;
	}

	errno = 0;
	rc = readlinkat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->path),
			reply, IOF_MAX_PATH_LEN);

	if (rc < 0)
		out->rc = errno;
	else
		out->path = (crt_string_t)reply;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	return 0;
}

int iof_truncate_handler(crt_rpc_t *rpc)
{
	struct iof_truncate_in *in = NULL;
	struct iof_status_out *out = NULL;
	char new_path[IOF_MAX_PATH_LEN];

	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (!in->path) {
		IOF_LOG_ERROR("No input path");
		out->err = IOF_ERR_CART;
		goto out;
	}

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

	return 0;
}

int iof_ftruncate_handler(crt_rpc_t *rpc)
{
	struct iof_ftruncate_in *in;
	struct iof_status_out *out;
	struct ionss_file_handle *handle = NULL;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(in->gah));

	rc = ios_gah_get_info(gs, &in->gah, (void **)&handle);
	if (rc != IOS_SUCCESS || !handle) {
		out->err = IOF_GAH_INVALID;
		IOF_LOG_DEBUG("Failed to load fd from gah %p %d",
			      &in->gah, rc);
	}

	errno = 0;
	rc = ftruncate(handle->fd, in->len);

	if (rc)
		out->rc = errno;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	return 0;
}

int iof_chmod_handler(crt_rpc_t *rpc)
{
	struct iof_chmod_in *in;
	struct iof_status_out *out;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (!in->path) {
		IOF_LOG_ERROR("No input path");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (in->fs_id >= num_fs) {
		out->err = IOF_BAD_DATA;
		goto out;
	}

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

	return 0;
}

int iof_chmod_gah_handler(crt_rpc_t *rpc)
{
	struct iof_chmod_gah_in *in;
	struct iof_status_out *out;
	struct ionss_file_handle *handle = NULL;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(in->gah));

	rc = ios_gah_get_info(gs, &in->gah, (void **)&handle);
	if (rc != IOS_SUCCESS || !handle) {
		out->err = IOF_GAH_INVALID;
		IOF_LOG_DEBUG("Failed to load fd from gah %p %d",
			      &in->gah, rc);
		out->err = rc;
		goto out;
	}

	errno = 0;
	rc = fchmod(handle->fd, in->mode);

	if (rc)
		out->rc = errno;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	return 0;
}

int iof_rmdir_handler(crt_rpc_t *rpc)
{
	struct iof_string_in *in = NULL;
	struct iof_status_out *out = NULL;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (!in->path) {
		IOF_LOG_ERROR("No input path");
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_DEBUG("path %d %s", in->fs_id, in->path);

	if (in->fs_id >= num_fs) {
		out->err = IOF_BAD_DATA;
		goto out;
	}

	errno = 0;
	rc = unlinkat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->path),
		      AT_REMOVEDIR);

	if (rc)
		out->rc = errno;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	return 0;
}

int iof_unlink_handler(crt_rpc_t *rpc)
{
	struct iof_string_in *in;
	struct iof_status_out *out;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_DEBUG("path %d %s", in->fs_id, in->path);

	if (in->fs_id >= num_fs) {
		out->err = IOF_BAD_DATA;
		goto out;
	}

	if (!in->path) {
		out->err = IOF_BAD_DATA;
		goto out;
	}

	errno = 0;
	rc = unlinkat(ID_TO_FD(in->fs_id), iof_get_rel_path(in->path), 0);

	if (rc)
		out->rc = errno;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	return 0;
}

int iof_write_direct_handler(crt_rpc_t *rpc)
{
	struct iof_write_in *in;
	struct iof_write_out *out;
	struct ionss_file_handle *handle = NULL;
	size_t bytes_written;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(in->gah));

	rc = ios_gah_get_info(gs, &in->gah, (void **)&handle);
	if (rc != IOS_SUCCESS || !handle) {
		out->err = IOF_GAH_INVALID;
		IOF_LOG_DEBUG("Failed to load fd from gah %p %d",
			      &in->gah, rc);
	}

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

	return 0;
}

int iof_write_bulk(const struct crt_bulk_cb_info *cb_info)
{
	struct iof_write_bulk *in;
	struct iof_write_out *out;
	size_t bytes_written;
	struct ionss_file_handle *handle = NULL;
	crt_iov_t iov = {0};
	crt_sg_list_t sgl = {0};

	int rc;

	out = crt_reply_get(cb_info->bci_bulk_desc->bd_rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	if (cb_info->bci_rc) {
		out->err = IOF_ERR_CART;
		goto out;
	}

	in = crt_req_get(cb_info->bci_bulk_desc->bd_rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	rc = ios_gah_get_info(gs, &in->gah, (void **)&handle);
	if (rc != IOS_SUCCESS || !handle) {
		out->err = IOF_GAH_INVALID;
		IOF_LOG_DEBUG("Failed to load fd from gah %p %d",
			      &in->gah, rc);
		goto out;
	}

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

	return 0;
}

int iof_write_bulk_handler(crt_rpc_t *rpc)
{
	struct iof_write_bulk *in;
	struct iof_write_out *out;
	struct ionss_file_handle *handle = NULL;
	struct crt_bulk_desc bulk_desc = {0};
	crt_bulk_t local_bulk_hdl = {0};
	crt_sg_list_t sgl = {0};
	crt_iov_t iov = {0};
	crt_size_t len;

	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_INFO(GAH_PRINT_STR, GAH_PRINT_VAL(in->gah));

	rc = ios_gah_get_info(gs, &in->gah, (void **)&handle);
	if (rc != IOS_SUCCESS || !handle) {
		out->err = IOF_GAH_INVALID;
		IOF_LOG_DEBUG("Failed to load fd from gah %p %d",
			      &in->gah, rc);
		goto out;
	}

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

	iov.iov_buf = malloc(len);
	if (!iov.iov_buf) {
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
	return 0;

out_decref:
	rc = crt_req_decref(rpc);

	if (rc)
		IOF_LOG_ERROR("Unable to drop reference, ret = %u", rc);
out:
	rc = crt_reply_send(rpc);

	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	return 0;
}

int iof_utimens_handler(crt_rpc_t *rpc)
{
	struct iof_time_in *in;
	struct iof_status_out *out;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (!in->path) {
		IOF_LOG_ERROR("No input path");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (!in->time.iov_buf) {
		IOF_LOG_ERROR("No input times");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (in->fs_id >= num_fs) {
		out->err = IOF_BAD_DATA;
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

	return 0;
}

int iof_utimens_gah_handler(crt_rpc_t *rpc)
{
	struct iof_time_gah_in *in;
	struct iof_status_out *out;
	struct ionss_file_handle *handle = NULL;
	int rc;

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out;
	}

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		out->err = IOF_ERR_CART;
		goto out;
	}

	if (!in->time.iov_buf) {
		IOF_LOG_ERROR("No input times");
		out->err = IOF_ERR_CART;
		goto out;
	}

	{
		char *d = ios_gah_to_str(&in->gah);

		IOF_LOG_INFO("Setting time of %s", d);
		free(d);
	}

	rc = ios_gah_get_info(gs, &in->gah, (void **)&handle);
	if (rc != IOS_SUCCESS || !handle) {
		out->err = IOF_GAH_INVALID;
		IOF_LOG_DEBUG("Failed to load fd from gah %p %d",
			      &in->gah, rc);
	}

	errno = 0;
	rc = futimens(handle->fd, in->time.iov_buf);

	if (rc)
		out->rc = errno;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	return 0;
}

/*
 * Process filesystem query from CNSS
 * This function currently uses dummy data to send back to CNSS
 */
int iof_query_handler(crt_rpc_t *query_rpc)
{
	struct iof_psr_query *query = NULL;
	int ret;

	query = crt_reply_get(query_rpc);
	if (query == NULL) {
		IOF_LOG_ERROR("could not get reply buffer");
		return IOF_ERR_CART;
	}

	crt_iov_set(&query->query_list, fs_list,
			num_fs * sizeof(struct iof_fs_info));

	ret = crt_reply_send(query_rpc);
	if (ret)
		IOF_LOG_ERROR("query rpc response not sent, ret = %d", ret);
	return ret;
}

int ionss_register(void)
{
	int ret;

	ret = crt_rpc_srv_register(QUERY_PSR_OP, &QUERY_RPC_FMT,
			iof_query_handler);
	if (ret) {
		IOF_LOG_ERROR("Can not register query RPC, ret = %d", ret);
		return ret;
	}

	ret = crt_rpc_srv_register(SHUTDOWN_OP, NULL, shutdown_handler);
	if (ret) {
		IOF_LOG_ERROR("Can not register shutdown RPC, ret = %d", ret);
		return ret;
	}

#define PROTO_SET_FUNCTION(PROTO, NAME, FN) (PROTO)->mt.NAME.fn = FN

	proto = iof_register();
	PROTO_SET_FUNCTION(proto, opendir, iof_opendir_handler);
	PROTO_SET_FUNCTION(proto, readdir, iof_readdir_handler);
	PROTO_SET_FUNCTION(proto, closedir, iof_closedir_handler);
	PROTO_SET_FUNCTION(proto, getattr, iof_getattr_handler);
	PROTO_SET_FUNCTION(proto, getattr_gah, iof_getattr_gah_handler);
	PROTO_SET_FUNCTION(proto, write_direct, iof_write_direct_handler);
	PROTO_SET_FUNCTION(proto, write_bulk, iof_write_bulk_handler);
	PROTO_SET_FUNCTION(proto, truncate, iof_truncate_handler);
	PROTO_SET_FUNCTION(proto, ftruncate, iof_ftruncate_handler);
	PROTO_SET_FUNCTION(proto, chmod, iof_chmod_handler);
	PROTO_SET_FUNCTION(proto, chmod_gah, iof_chmod_gah_handler);
	PROTO_SET_FUNCTION(proto, rmdir, iof_rmdir_handler);
	PROTO_SET_FUNCTION(proto, rename, iof_rename_handler);
	PROTO_SET_FUNCTION(proto, read_bulk, iof_read_bulk_handler);
	PROTO_SET_FUNCTION(proto, unlink, iof_unlink_handler);
	PROTO_SET_FUNCTION(proto, open, iof_open_handler);
	PROTO_SET_FUNCTION(proto, create, iof_create_handler);
	PROTO_SET_FUNCTION(proto, read, iof_read_handler);
	PROTO_SET_FUNCTION(proto, close, iof_close_handler);
	PROTO_SET_FUNCTION(proto, mkdir, iof_mkdir_handler);
	PROTO_SET_FUNCTION(proto, readlink, iof_readlink_handler);
	PROTO_SET_FUNCTION(proto, symlink, iof_symlink_handler);
	PROTO_SET_FUNCTION(proto, fsync, iof_fsync_handler);
	PROTO_SET_FUNCTION(proto, fdatasync, iof_fdatasync_handler);
	PROTO_SET_FUNCTION(proto, utimens, iof_utimens_handler);
	PROTO_SET_FUNCTION(proto, utimens_gah, iof_utimens_gah_handler);
	iof_proto_commit(proto);

	return ret;
}

static void *progress_thread(void *arg)
{
	int			rc;
	crt_context_t		crt_ctx;

	crt_ctx = (crt_context_t) arg;
	/* progress loop */
	do {
		rc = crt_progress(crt_ctx, 1, NULL, NULL);
		if (rc != 0 && rc != -CER_TIMEDOUT) {
			IOF_LOG_ERROR("crt_progress failed rc: %d", rc);
			break;
		}

	} while (!shutdown);

	IOF_LOG_DEBUG("progress_thread exiting");

	pthread_exit(NULL);
}

int main(int argc, char **argv)
{
	char *ionss_grp = "IONSS";
	crt_context_t crt_ctx;
	int i;
	int ret = IOF_SUCCESS;
	pthread_t progress_tid;
	int err;

	char *version = iof_get_version();

	iof_log_init("ION", "IONSS");
	IOF_LOG_INFO("IONSS version: %s", version);
	if (argc < 2) {
		IOF_LOG_ERROR("Expected at least one directory as command line option");
		return IOF_BAD_DATA;
	}
	num_fs = argc - 1;
	/*hardcoding the number and path for projected filesystems*/
	fs_list = calloc(num_fs, sizeof(*fs_list));
	if (!fs_list) {
		IOF_LOG_ERROR("Filesystem list not allocated");
		ret = IOF_ERR_NOMEM;
		goto cleanup;
	}

	projections = calloc(num_fs, sizeof(*projections));
	if (!projections) {
		IOF_LOG_ERROR("Filesystem list not allocated");
		ret = IOF_ERR_NOMEM;
		goto cleanup;
	}

	IOF_LOG_INFO("Projecting %d exports", num_fs);

	/*
	 * Check each export location.
	 *
	 * Exports must be directories.
	 * Exports are identified by the absolute path, without allowing for
	 * symbolic links.
	 * The maximum path length of exports is checked.
	 */
	err = 0;
	for (i = 0; i < num_fs; i++) {
		struct ionss_projection *projection = &projections[i];
		struct stat buf = {0};
		char *full_path = realpath(argv[i + 1], NULL);
		int rc;

		if (!full_path) {
			IOF_LOG_ERROR("Export path does not exist: %s",
				      argv[i + 1]);
			err = 1;
			continue;
		}

		rc = stat(full_path, &buf);
		if (rc) {
			IOF_LOG_ERROR("Could not stat export path %s %d",
				      full_path, errno);
			err = 1;
			continue;
		}

		if (!S_ISDIR(buf.st_mode)) {
			IOF_LOG_ERROR("Export path is not a directory %s",
				      full_path);
			err = 1;
			continue;
		}

		projection->dir = opendir(full_path);
		if (!projection->dir) {
			IOF_LOG_ERROR("Could not open export directory %s",
				      full_path);
			err = 1;
			continue;
		}
		projection->dir_fd = dirfd(projection->dir);

		if (strnlen(full_path, IOF_MAX_PATH_LEN - 1) ==
			(IOF_MAX_PATH_LEN - 1)) {
			IOF_LOG_ERROR("Export path is too deep %s",
				      full_path);
			err = 1;
			continue;
		}

		IOF_LOG_INFO("Projecting %s", full_path);
		fs_list[i].mode = 0;
		fs_list[i].id = i;
		strncpy(fs_list[i].mnt, full_path, IOF_NAME_LEN_MAX);
		free(full_path);
	}
	if (err)
		return 1;

	/*initialize CaRT*/
	ret = crt_init(ionss_grp, CRT_FLAG_BIT_SERVER);
	if (ret) {
		IOF_LOG_ERROR("Crt_init failed with ret = %d", ret);
		return ret;
	}

	ret = crt_context_create(NULL, &crt_ctx);
	if (ret) {
		IOF_LOG_ERROR("Could not create context");
		goto cleanup;
	}
	ionss_register();

	gs = ios_gah_init();

	shutdown = 0;
	ret = pthread_create(&progress_tid, NULL, progress_thread, crt_ctx);

	ret = pthread_join(progress_tid, NULL);
	if (ret)
		IOF_LOG_ERROR("Could not join progress thread");

	ret = crt_context_destroy(crt_ctx, 0);
	if (ret)
		IOF_LOG_ERROR("Could not destroy context");

	ret = crt_finalize();
	if (ret)
		IOF_LOG_ERROR("Could not finalize cart");

	ios_gah_destroy(gs);
cleanup:
	if (fs_list)
		free(fs_list);

	for (i = 0; i < num_fs; i++) {
		struct ionss_projection *projection = &projections[i];

		if (projection->dir)
			closedir(projection->dir);
	}

	if (projections)
		free(projections);

	iof_log_close();

	return ret;
}
