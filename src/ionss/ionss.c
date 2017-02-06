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

static struct iof_fs_info *fs_list;
static struct ios_gah_store *gs;
static int num_fs;
static int shutdown;

#define IONSS_READDIR_ENTRIES_PER_RPC (2)

struct ionss_dir_handle {
	int	fs_id;
	char	*h_name;
	DIR	*h_dir;
	int	fd;
};

struct ionss_file_handle {
	int	fs_id;
	int	fd;
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
	char new_path[IOF_MAX_PATH_LEN];
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

	if (!in->path) {
		IOF_LOG_ERROR("No input path");
		out->err = IOF_ERR_CART;
		goto out;
	}

	IOF_LOG_DEBUG("Checking path %s", in->path);
	rc = iof_get_path(in->my_fs_id, in->path, &new_path[0]);
	if (rc) {
		IOF_LOG_ERROR("could not construct filesystem path, rc = %u",
			      rc);
		out->err = rc;
		goto out;
	}

	errno = 0;
	rc = lstat(new_path, &stbuf);
	if (rc)
		out->rc = errno;
	else
		crt_iov_set(&out->stat, &stbuf, sizeof(struct stat));

out:

	IOF_LOG_DEBUG("path %s result err %d rc %d",
		      in->path, out->err, out->rc);

	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, rc = %u", rc);

	return 0;
}

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

	{
		char *d = ios_gah_to_str(&in->gah);

		IOF_LOG_INFO("Reading from %s", d);
		free(d);
	}

	rc = ios_gah_get_info(gs, &in->gah, (void **)&handle);
	if (rc != IOS_SUCCESS || !handle) {
		out->err = IOF_GAH_INVALID;
		IOF_LOG_DEBUG("Failed to load fd from gah %p %d",
			      &in->gah, rc);
		goto out;
	}

	errno = 0;
	rc = fstat(handle->fd, &stbuf);
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
	struct iof_string_in *in = NULL;
	struct iof_opendir_out *out = NULL;
	char new_path[IOF_MAX_PATH_LEN];
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

	IOF_LOG_DEBUG("Checking path %s", in->path);
	rc = iof_get_path(in->my_fs_id, in->path, &new_path[0]);
	if (rc) {
		IOF_LOG_ERROR("could not construct filesystem path, rc = %u",
			      rc);
		out->err = rc;
	} else {
		DIR *dir_h;
		struct ios_gah gah;

		out->err = 0;
		dir_h = opendir(new_path);
		if (dir_h) {
			char *s;
			struct ionss_dir_handle *h;

			h = malloc(sizeof(struct ionss_dir_handle));

			h->h_dir = dir_h;
			h->fd = dirfd(h->h_dir);
			h->h_name = strdup(in->path);
			h->fs_id = in->my_fs_id;

			ios_gah_allocate(gs, &gah, 0, 0, h);
			s = ios_gah_to_str(&gah);
			IOF_LOG_INFO("Allocated %s", s);
			IOF_LOG_DEBUG("Dirp is %p", dir_h);
			free(s);

			out->rc = 0;
			out->gah = gah;
		} else {
			out->rc = errno;
		}
	}

	IOF_LOG_DEBUG("path %s result err %d rc %d",
		      in->path, out->err, out->rc);

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
	char *gah_d;
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

	gah_d = ios_gah_to_str(&in->gah);
	IOF_LOG_INFO("Reading from %s", gah_d);
	free(gah_d);

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
	char *d;
	int rc;

	in = crt_req_get(rpc);
	if (!in) {
		IOF_LOG_ERROR("Could not retrieve input args");
		return 0;
	}

	d = ios_gah_to_str(&in->gah);
	IOF_LOG_INFO("Deallocating %s", d);
	free(d);

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

int iof_open_handler(crt_rpc_t *rpc)
{
	struct iof_string_in *in;
	struct iof_open_out *out;
	struct ionss_file_handle *local_handle;
	struct ios_gah gah = {0};
	int fd;
	int rc;
	char new_path[IOF_MAX_PATH_LEN];

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

	IOF_LOG_DEBUG("path %s", in->path);

	rc = iof_get_path(in->my_fs_id, in->path, &new_path[0]);
	if (rc) {
		IOF_LOG_ERROR("could not construct filesystem path, rc = %d",
			      rc);
		out->err = rc;
		goto out;
	}

	errno = 0;
	fd = open(new_path, O_RDWR);
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
	local_handle->fs_id = in->my_fs_id;

	rc = ios_gah_allocate(gs, &gah, 0, 0, local_handle);
	if (rc != IOS_SUCCESS) {
		close(fd);
		free(local_handle);
		out->err = IOF_ERR_INTERNAL;
		goto out;
	}

	{
		char *s = ios_gah_to_str(&gah);

		IOF_LOG_INFO("Allocated %s", s);
		free(s);
	}

	out->gah = gah;

out:
	IOF_LOG_INFO("path %s result err %d rc %d handle %p",
		     in->path, out->err, out->rc, local_handle);

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
	char new_path[IOF_MAX_PATH_LEN];

	out = crt_reply_get(rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not retrieve output args");
		goto out_no_log;
		return 0;
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

	IOF_LOG_DEBUG("path %s", in->path);

	rc = iof_get_path(in->my_fs_id, in->path, &new_path[0]);
	if (rc) {
		IOF_LOG_ERROR("could not construct filesystem path, rc = %d",
			      rc);
		out->err = rc;
		goto out;
	}

	errno = 0;
	fd = creat(new_path, in->mode);
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
	local_handle->fs_id = in->my_fs_id;

	rc = ios_gah_allocate(gs, &gah, 0, 0, local_handle);
	if (rc != IOS_SUCCESS) {
		close(fd);
		free(local_handle);
		out->err = IOF_ERR_INTERNAL;
		goto out;
	}

	{
		char *s = ios_gah_to_str(&gah);

		IOF_LOG_INFO("Allocated %s", s);
		free(s);
	}

	out->gah = gah;

out:
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

	{
		char *d = ios_gah_to_str(&in->gah);

		IOF_LOG_INFO("Deallocating %s", d);
		free(d);
	}

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

	{
		char *d = ios_gah_to_str(&in->gah);

		IOF_LOG_INFO("Calling fsync %s", d);
		free(d);
	}

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

	{
		char *d = ios_gah_to_str(&in->gah);

		IOF_LOG_INFO("Calling fdatasync %s", d);
		free(d);
	}

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

	{
		char *d = ios_gah_to_str(&in->gah);

		IOF_LOG_INFO("Reading from %s", d);
		free(d);
	}

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

	{
		char *d = ios_gah_to_str(&in->gah);

		IOF_LOG_INFO("Reading from %s", d);
		free(d);
	}

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
	struct iof_two_string_in *in = NULL;
	struct iof_status_out *out = NULL;
	char new_src[IOF_MAX_PATH_LEN];
	char new_dst[IOF_MAX_PATH_LEN];
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

	rc = iof_get_path(in->my_fs_id, in->src, &new_src[0]);
	if (rc) {
		IOF_LOG_ERROR("could not construct filesystem path, ret = %u",
			      rc);
		out->err = rc;
		goto out;
	}

	rc = iof_get_path(in->my_fs_id, in->dst, &new_dst[0]);
	if (rc) {
		IOF_LOG_ERROR("could not construct filesystem path, ret = %u",
			      rc);
		out->err = rc;
		goto out;
	}

	rc = rename(new_src, new_dst);

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
	char new_src[IOF_MAX_PATH_LEN];
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

	rc = iof_get_path(in->my_fs_id, in->src, &new_src[0]);
	if (rc) {
		IOF_LOG_ERROR("could not construct filesystem path, rc = %u",
			      rc);
		out->err = rc;
		goto out;
	}

	rc = symlink(in->dst, new_src);

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

int iof_mkdir_handler(crt_rpc_t *rpc)
{
	struct iof_create_in *in;
	struct iof_status_out *out;
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

	rc = iof_get_path(in->my_fs_id, in->path, &new_path[0]);
	if (rc) {
		IOF_LOG_ERROR("could not construct filesystem path, rc = %d",
			      rc);
		out->err = rc;
		goto out;
	}

	errno = 0;
	rc = mkdir(new_path, in->mode);

	if (rc)
		out->rc = errno;

out:
	rc = crt_reply_send(rpc);
	if (rc)
		IOF_LOG_ERROR("response not sent, ret = %u", rc);

	return 0;
}

int iof_readlink_handler(crt_rpc_t *rpc)
{
	struct iof_string_in *in;
	struct iof_string_out *out;
	char new_path[IOF_MAX_PATH_LEN];
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

	rc = iof_get_path(in->my_fs_id, in->path, &new_path[0]);
	if (rc) {
		IOF_LOG_ERROR("could not construct filesystem path, rc = %d",
			      rc);
		out->err = rc;
		goto out;
	}

	errno = 0;
	rc = readlink(new_path, reply, IOF_MAX_PATH_LEN);

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

	rc = iof_get_path(in->my_fs_id, in->path, &new_path[0]);
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

	{
		char *d = ios_gah_to_str(&in->gah);

		IOF_LOG_INFO("Writing to %s", d);
		free(d);
	}

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

	rc = iof_get_path(in->my_fs_id, in->path, &new_path[0]);
	if (rc) {
		IOF_LOG_ERROR("could not construct filesystem path, rc = %d",
			      rc);
		out->err = rc;
		goto out;
	}

	errno = 0;
	rc = chmod(new_path, in->mode);

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

	{
		char *d = ios_gah_to_str(&in->gah);

		IOF_LOG_INFO("Writing to %s", d);
		free(d);
	}

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

	rc = iof_get_path(in->my_fs_id, in->path, &new_path[0]);
	if (rc) {
		IOF_LOG_ERROR("could not construct filesystem path, rc = %d",
			      rc);
		out->err = rc;
		goto out;
	}

	errno = 0;
	rc = rmdir(new_path);

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

	rc = iof_get_path(in->my_fs_id, in->path, &new_path[0]);
	if (rc) {
		IOF_LOG_ERROR("could not construct filesystem path, rc = %d",
			      rc);
		out->err = rc;
		goto out;
	}

	errno = 0;
	rc = unlink(new_path);

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

	{
		char *d = ios_gah_to_str(&in->gah);

		IOF_LOG_INFO("Writing to %s", d);
		free(d);
	}

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

	{
		char *d = ios_gah_to_str(&in->gah);

		IOF_LOG_INFO("Writing to %s", d);
		free(d);
	}

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
	fs_list = calloc(num_fs, sizeof(struct iof_fs_info));
	if (!fs_list) {
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

	iof_log_close();

	return ret;
}
