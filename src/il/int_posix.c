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
#include <stdarg.h>
#include <inttypes.h>
#include <libgen.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <string.h>
#include <crt_util/list.h>
#include <crt_api.h>
#include "iof_mntent.h"
#include "intercept.h"
#include "iof_ioctl.h"
#include "iof_vector.h"
#include "iof_common.h"

FOREACH_INTERCEPT(IOIL_FORWARD_DECL)

bool ioil_initialized;
static vector_t fd_table;
static char *ctrl_prefix;
static char *cnss_prefix;
static char *cnss_env;
static crt_group_t *ionss_grp;
static crt_context_t context;

#define BLOCK_SIZE 1024

struct fd_entry {
	struct ios_gah gah;
	off_t pos;
	int flags;
	bool disabled;
};

static int iof_check_complete(void *arg)
{
	int *complete = (int *)arg;
	return *complete;
}

/* on-demand progress */
static int iof_progress(crt_context_t crt_ctx, int *complete_flag)
{
	int		rc;

	do {
		rc = crt_progress(crt_ctx, 1000 * 1000, iof_check_complete,
				  complete_flag);

		if (*complete_flag)
			return 0;

	} while (rc == 0 || rc == -CER_TIMEDOUT);

	IOF_LOG_ERROR("crt_progress failed rc: %d", rc);
	return -1;
}

/* Progress, from within ioil entry point during normal I/O */
int ioil_cb_progress(struct file_info *f_info, int *complete_flag)
{
	int rc;

	rc = iof_progress(f_info->crt_ctx, complete_flag);
	if (rc) {
		IOF_LOG_ERROR("Progress loop exited, rc %d", rc);
		return -1;
	}
	return 0;
}


int ioil_initialize_fd_table(int max_fds)
{
	int rc;

	rc = vector_init(&fd_table, sizeof(struct fd_entry), max_fds);

	if (rc != 0)
		IOF_LOG_ERROR("Could not allocated file descriptor table"
			      ", disabling interception: rc = %d", rc);
	return rc;
}

static int check_mnt(struct mntent *entry, void *priv)
{
	char *ctrl_dir;
	char *dir;
	char *cnss_dir;
	char *p;
	struct stat buf;
	int rc;

	p = strstr(entry->mnt_dir, "/.ctrl");
	if (p == NULL || strcmp(entry->mnt_type, "fuse.ctrl") ||
	    strcmp(entry->mnt_fsname, "CNSS"))
		return 0;

	IOF_LOG_INFO("Checking possible IONSS: ctrl dir at %s", entry->mnt_dir);
	rc = stat(entry->mnt_dir, &buf);
	if (rc != 0) {
		IOF_LOG_INFO("Skipping IONSS: ctrl dir is %s: %s",
			     entry->mnt_dir, strerror(errno));
		return 0;
	}

	dir = strdup(entry->mnt_dir);
	if (dir == NULL) {
		IOF_LOG_ERROR("Insufficient memory to configure interception");
		return 0;
	}

	ctrl_dir = strdup(entry->mnt_dir);
	if (ctrl_dir == NULL) {
		IOF_LOG_ERROR("Insufficient memory to configure interception");
		free(dir);
		return 0;
	}

	cnss_dir = strdup(dirname(dir));
	if (cnss_dir == NULL) {
		IOF_LOG_ERROR("Insufficient memory to configure interception");
		free(dir);
		free(ctrl_dir);
		return 0;
	}

	free(dir);

	if (cnss_env != NULL && strcmp(cnss_dir, cnss_env)) {
		IOF_LOG_INFO("Skipping IONSS: CNSS_PREFIX doesn't match");
		free(ctrl_dir);
		free(cnss_dir);
		return 0;
	}

	rc = crt_group_config_path_set(cnss_dir);
	if (rc != 0) {
		IOF_LOG_INFO("Could not set group config path, rc = %d", rc);
		free(cnss_dir);
		free(ctrl_dir);
		return 0;
	}

	/* Ok, now try to attach.  Note, this will change when we
	 * attach to multiple IONSS processes
	 */
	rc = crt_group_attach("IONSS", &ionss_grp);
	if (rc != 0) {
		IOF_LOG_INFO("Could not attach to ionss, rc = %d", rc);
		free(cnss_dir);
		free(ctrl_dir);
		return 0;
	}

	if (ctrl_prefix != NULL) {
		IOF_LOG_ERROR("Multiple viable IOF options not supported");
		goto handle_error;
	}

	cnss_prefix = cnss_dir;
	ctrl_prefix = ctrl_dir;

	return 0;
handle_error:
	crt_group_detach(ionss_grp);
	free(ctrl_dir);
	free(cnss_dir);
	if (ctrl_prefix != NULL &&  ctrl_dir != ctrl_prefix) {
		free(ctrl_prefix);
		ctrl_prefix = NULL;
	}
	if (cnss_prefix != NULL &&  cnss_dir != cnss_prefix) {
		free(cnss_prefix);
		cnss_prefix = NULL;
	}

	return 0;
}

static ssize_t pread_rpc(struct fd_entry *entry, char *buff, size_t len,
			 off_t offset, int *err)
{
	ssize_t bytes_read;
	struct file_info fi;

	/* Just get rpc working then work out how to really do this */
	fi.crt_ctx = context;
	fi.dest_ep.ep_grp = ionss_grp;
	fi.dest_ep.ep_rank = entry->gah.base;
	fi.dest_ep.ep_tag = 0;
	fi.gah = entry->gah;
	fi.gah_valid = true;
	fi.errcode = 0;

	bytes_read = ioil_do_pread(buff, len, offset, &fi);
	if (bytes_read < 0)
		*err = fi.errcode;
	return bytes_read;
}

/* Start simple and just loop */
static ssize_t preadv_rpc(struct fd_entry *entry, const struct iovec *iov,
			  int count, off_t offset, int *err)
{
	ssize_t bytes_read;
	struct file_info fi;

	/* Just get rpc working then work out how to really do this */
	fi.crt_ctx = context;
	fi.dest_ep.ep_grp = ionss_grp;
	fi.dest_ep.ep_rank = entry->gah.base;
	fi.dest_ep.ep_tag = 0;
	fi.gah = entry->gah;
	fi.gah_valid = true;
	fi.errcode = 0;

	bytes_read = ioil_do_preadv(iov, count, offset, &fi);
	if (bytes_read < 0)
		*err = fi.errcode;
	return bytes_read;
}

static __attribute__((constructor)) void ioil_init(void)
{
	struct rlimit rlimit;
	int rc;

	FOREACH_INTERCEPT(IOIL_FORWARD_MAP_OR_FAIL);

	iof_log_init("IL", "IOIL");

	/* Get maximum number of file desciptors */
	rc = getrlimit(RLIMIT_NOFILE, &rlimit);
	if (rc != 0) {
		IOF_LOG_ERROR("Could not get process file descriptor limit"
			      ", disabling interception");
		return;
	}

	rc = ioil_initialize_fd_table(rlimit.rlim_max);
	if (rc != 0) {
		IOF_LOG_ERROR("Could not create fd_table, rc = %d,"
			      ", disabling interception", rc);
		return;
	}

	rc = crt_init(NULL, CRT_FLAG_BIT_SINGLETON);
	if (rc != 0) {
		IOF_LOG_ERROR("Could not initialize crt, rc = %d,"
			      " disabling interception", rc);
		return;
	}

	rc = crt_context_create(NULL, &context);
	if (rc != 0) {
		IOF_LOG_ERROR("Could not create crt context, rc = %d,"
			      " disabling interception", rc);
		crt_finalize();
		return;
	}

	rc = iof_register(DEF_PROTO_CLASS(DEFAULT), NULL);
	if (rc != 0) {
		crt_context_destroy(context, 0);
		crt_finalize();
		IOF_LOG_ERROR("Could not create crt context, rc = %d,"
			      " disabling interception", rc);
		return;
	}

	cnss_env = getenv("CNSS_PREFIX");

	iof_mntent_foreach(check_mnt, NULL);

	if (cnss_prefix == NULL) {
		if (cnss_env != NULL)
			IOF_LOG_ERROR("CNSS_PREFIX is set but indicates"
				      " invalid IOF session. Is it set by "
				      "mistake?  Disabling interception");
		else
			IOF_LOG_ERROR("Could not detect active IOF session"
				      ", disabling interception");
		return;
	}

	IOF_LOG_INFO("Using IONSS: ctrl dir at %s", ctrl_prefix);

	__sync_synchronize();

	ioil_initialized = true;
}

static __attribute__((destructor)) void ioil_fini(void)
{
	if (ioil_initialized) {
		crt_group_detach(ionss_grp);
		crt_context_destroy(context, 0);
		crt_finalize();
	}
	ioil_initialized = false;

	__sync_synchronize();

	iof_log_close();

	free(cnss_prefix);
	free(ctrl_prefix);

	vector_destroy(&fd_table);
}

static void check_ioctl_on_open(int fd, int flags)
{
	struct fd_entry entry;
	int saved_errno;
	int rc;

	if (fd == -1)
		return;

	saved_errno = errno; /* Save the errno from open */

	rc = ioctl(fd, IOF_IOCTL_GAH, &entry.gah);
	if (rc == 0) {
		IOIL_LOG_INFO("Opened an IOF file (fd = %d) "
			      GAH_PRINT_STR, fd, GAH_PRINT_VAL(entry.gah));
		entry.pos = 0;
		entry.flags = flags;
		entry.disabled = false;
		rc = vector_set(&fd_table, fd, &entry);
		if (rc != 0)
			IOIL_LOG_INFO("Failed to insert gah in table, rc = %d",
				     rc);
	}

	errno = saved_errno; /* Restore the errno from open */
}

static bool drop_reference_if_disabled(int fd, struct fd_entry *entry)
{
	if (!entry->disabled)
		return false;

	IOIL_LOG_INFO("Dropped reference to disabled file " GAH_PRINT_STR,
		     GAH_PRINT_VAL(entry->gah));
	vector_remove(&fd_table, fd, NULL);
	vector_decref(&fd_table, entry);

	return true;
}

IOIL_PUBLIC int IOIL_DECL(open)(const char *pathname, int flags, ...)
{
	int fd;
	unsigned int mode; /* mode_t gets "promoted" to unsigned int
			    * for va_arg routine
			    */

	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = va_arg(ap, unsigned int);
		va_end(ap);

		IOIL_LOG_INFO("open(%s, 0%o, 0%o) intercepted",
			     pathname, flags, mode);

		fd = __real_open(pathname, flags, mode);
	} else {
		IOIL_LOG_INFO("open(%s, 0%o) intercepted", pathname, flags);

		fd =  __real_open(pathname, flags);
	}

	/* Ignore O_APPEND files for now */
	if (ioil_initialized && ((flags & O_APPEND) == 0))
		check_ioctl_on_open(fd, flags);

	return fd;
}

IOIL_PUBLIC int IOIL_DECL(creat)(const char *pathname, mode_t mode)
{
	int fd;

	IOIL_LOG_INFO("creat(%s, 0%o) intercepted", pathname, mode);

	/* Same as open with O_CREAT|O_WRONLY|O_TRUNC */
	fd = __real_open(pathname, O_CREAT|O_WRONLY|O_TRUNC, mode);

	if (ioil_initialized)
		check_ioctl_on_open(fd, O_CREAT|O_WRONLY|O_TRUNC);

	return fd;
}

IOIL_PUBLIC int IOIL_DECL(close)(int fd)
{
	struct fd_entry *entry;
	int rc;

	IOIL_LOG_INFO("close(%d) intercepted", fd);

	rc = vector_remove(&fd_table, fd, &entry);

	if (rc == 0) {
		IOIL_LOG_INFO("Removed IOF entry for fd=%d "
			      GAH_PRINT_STR, fd, GAH_PRINT_VAL(entry->gah));
		vector_decref(&fd_table, entry);
	}

	return __real_close(fd);
}

IOIL_PUBLIC ssize_t IOIL_DECL(read)(int fd, void *buf, size_t len)
{
	struct fd_entry *entry;
	ssize_t bytes_read;
	off_t oldpos;
	int err;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_read(fd, buf, len);

	IOIL_LOG_INFO("read(%d, %p, %zu) intercepted " GAH_PRINT_STR,
		      fd, buf, len, GAH_PRINT_VAL(entry->gah));

	oldpos = entry->pos;
	bytes_read = pread_rpc(entry, buf, len, oldpos, &err);
	if (bytes_read > 0)
		entry->pos = oldpos + bytes_read;
	vector_decref(&fd_table, entry);

	if (bytes_read < 0)
		errno = err;
	return bytes_read;
}

IOIL_PUBLIC ssize_t IOIL_DECL(pread)(int fd, void *buf,
				     size_t len, off_t offset)
{
	struct fd_entry *entry;
	ssize_t bytes_read;
	int err;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_pread(fd, buf, len, offset);

	IOIL_LOG_INFO("pread(%d, %p, %zu, %zd) intercepted " GAH_PRINT_STR, fd,
		      buf, len, offset, GAH_PRINT_VAL(entry->gah));

	bytes_read = pread_rpc(entry, buf, len, offset, &err);

	vector_decref(&fd_table, entry);

	if (bytes_read < 0)
		errno = err;

	return bytes_read;
}

IOIL_PUBLIC ssize_t IOIL_DECL(write)(int fd, const void *buf, size_t len)
{
	struct fd_entry *entry;
	ssize_t bytes_written;
	off_t oldpos;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_write(fd, buf, len);

	IOIL_LOG_INFO("write(%d, %p, %zu) intercepted " GAH_PRINT_STR,
		      fd, buf, len, GAH_PRINT_VAL(entry->gah));

	oldpos = entry->pos;
	bytes_written = __real_pwrite(fd, buf, len, entry->pos);
	if (bytes_written > 0)
		entry->pos = oldpos + bytes_written;
	vector_decref(&fd_table, entry);

	return bytes_written;
}

IOIL_PUBLIC ssize_t IOIL_DECL(pwrite)(int fd, const void *buf,
					size_t len, off_t offset)
{
	struct fd_entry *entry;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_pwrite(fd, buf, len, offset);

	IOIL_LOG_INFO("pwrite(%d, %p, %zu, %zd) intercepted " GAH_PRINT_STR, fd,
		      buf, len, offset, GAH_PRINT_VAL(entry->gah));
	vector_decref(&fd_table, entry);

	return __real_pwrite(fd, buf, len, offset);
}

IOIL_PUBLIC off_t IOIL_DECL(lseek)(int fd, off_t offset, int whence)
{
	struct fd_entry *entry;
	off_t new_offset = -1;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_lseek(fd, offset, whence);

	IOIL_LOG_INFO("lseek(%d, %zd, %d) intercepted " GAH_PRINT_STR, fd,
		      offset, whence, GAH_PRINT_VAL(entry->gah));

	if (whence == SEEK_SET)
		new_offset = offset;
	else if (whence == SEEK_CUR)
		new_offset = entry->pos + offset;
	else {
		/* Let the system handle SEEK_END as well as non-standard
		 * values such as SEEK_DATA and SEEK_HOLE
		 */
		new_offset = __real_lseek(fd, offset, whence);
		if (new_offset >= 0)
			entry->pos = new_offset;
		goto cleanup;
	}

	if (new_offset < 0) {
		new_offset = (off_t)-1;
		errno = EINVAL;
	} else
		entry->pos = new_offset;

cleanup:
	vector_decref(&fd_table, entry);

	return new_offset;
}

IOIL_PUBLIC ssize_t IOIL_DECL(readv)(int fd, const struct iovec *vector,
				     int count)
{
	struct fd_entry *entry;
	ssize_t bytes_read;
	off_t oldpos;
	int err;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_readv(fd, vector, count);

	IOIL_LOG_INFO("readv(%d, %p, %d) intercepted " GAH_PRINT_STR,
		      fd, vector, count, GAH_PRINT_VAL(entry->gah));

	oldpos = entry->pos;
	bytes_read = preadv_rpc(entry, vector, count, entry->pos, &err);
	if (bytes_read > 0)
		entry->pos = oldpos + bytes_read;
	vector_decref(&fd_table, entry);

	if (bytes_read < 0)
		errno = err;

	return bytes_read;
}

IOIL_PUBLIC ssize_t IOIL_DECL(preadv)(int fd, const struct iovec *vector,
				      int count, off_t offset)
{
	struct fd_entry *entry;
	ssize_t bytes_read;
	int err;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_preadv(fd, vector, count, offset);

	IOIL_LOG_INFO("preadv(%d, %p, %d, %zd) intercepted " GAH_PRINT_STR, fd,
		      vector, count, offset, GAH_PRINT_VAL(entry->gah));

	bytes_read = preadv_rpc(entry, vector, count, offset, &err);
	vector_decref(&fd_table, entry);

	if (bytes_read < 0)
		errno = err;

	return bytes_read;
}

IOIL_PUBLIC ssize_t IOIL_DECL(writev)(int fd, const struct iovec *vector,
				      int count)
{
	struct fd_entry *entry;
	ssize_t bytes_written;
	off_t oldpos;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_writev(fd, vector, count);

	IOIL_LOG_INFO("writev(%d, %p, %d) intercepted " GAH_PRINT_STR,
		      fd, vector, count, GAH_PRINT_VAL(entry->gah));

	oldpos = entry->pos;
	bytes_written = __real_pwritev(fd, vector, count, entry->pos);
	if (bytes_written > 0)
		entry->pos = oldpos + bytes_written;
	vector_decref(&fd_table, entry);

	return bytes_written;
}

IOIL_PUBLIC ssize_t IOIL_DECL(pwritev)(int fd, const struct iovec *vector,
					 int count, off_t offset)
{
	struct fd_entry *entry;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_pwritev(fd, vector, count, offset);

	IOIL_LOG_INFO("pwritev(%d, %p, %d, %zd) intercepted " GAH_PRINT_STR, fd,
		      vector, count, offset, GAH_PRINT_VAL(entry->gah));

	vector_decref(&fd_table, entry);
	return __real_pwritev(fd, vector, count, offset);
}

IOIL_PUBLIC void *IOIL_DECL(mmap)(void *address, size_t length, int protect,
				    int flags, int fd, off_t offset)
{
	struct fd_entry *entry;
	int rc;

	rc = vector_remove(&fd_table, fd, &entry);
	if (rc == 0) {
		IOIL_LOG_INFO("mmap(%p, %zu, %d, %d, %d, %zd) intercepted, "
			      "stopping interception " GAH_PRINT_STR, address,
			      length, protect, flags, fd, offset,
			      GAH_PRINT_VAL(entry->gah));

		if (entry->pos != 0)
			__real_lseek(fd, entry->pos, SEEK_SET);
		entry->disabled = true; /* Signal others to drop references */

		vector_decref(&fd_table, entry);
	}

	return __real_mmap(address, length, protect, flags, fd, offset);
}

IOIL_PUBLIC int IOIL_DECL(fsync)(int fd)
{
	struct fd_entry *entry;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_fsync(fd);

	IOIL_LOG_INFO("fsync(%d) intercepted " GAH_PRINT_STR, fd,
		      GAH_PRINT_VAL(entry->gah));
	vector_decref(&fd_table, entry);

	return __real_fsync(fd);
}

IOIL_PUBLIC int IOIL_DECL(fdatasync)(int fd)
{
	struct fd_entry *entry;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_fdatasync(fd);

	IOIL_LOG_INFO("fdatasync(%d) intercepted " GAH_PRINT_STR,
		      fd, GAH_PRINT_VAL(entry->gah));
	vector_decref(&fd_table, entry);

	return __real_fdatasync(fd);
}

IOIL_PUBLIC int IOIL_DECL(dup)(int fd)
{
	struct fd_entry *entry = NULL;
	int rc;
	int newfd = __real_dup(fd);

	if (newfd == -1)
		return -1;

	rc = vector_dup(&fd_table, fd, newfd, &entry);
	if (rc == 0 && entry != NULL) {
		IOIL_LOG_INFO("dup(%d) intercepted " GAH_PRINT_STR,
			      fd, GAH_PRINT_VAL(entry->gah));
		if (drop_reference_if_disabled(newfd, entry)) {
			/* If the file was disabled, get the duplicated
			 * entry and if it hasn't changed, drop its
			 * reference too
			 */
			rc = vector_get(&fd_table, fd, &entry);
			if (rc == 0) {
				if (!drop_reference_if_disabled(fd, entry))
					vector_decref(&fd_table, entry);
			}
		} else
			vector_decref(&fd_table, entry);
	}

	return newfd;
}

IOIL_PUBLIC int IOIL_DECL(dup2)(int old, int new)
{
	struct fd_entry *entry = NULL;
	int newfd = __real_dup2(old, new);
	int rc;

	if (newfd == -1)
		return -1;

	rc = vector_dup(&fd_table, old, newfd, &entry);
	if (rc == 0 && entry != NULL) {
		IOIL_LOG_INFO("dup2(%d, %d) intercepted " GAH_PRINT_STR,
			      old, new, GAH_PRINT_VAL(entry->gah));
		if (drop_reference_if_disabled(new, entry)) {
			/* If the file was disabled, get the duplicated
			 * entry and if it hasn't changed, drop its
			 * reference too
			 */
			rc = vector_get(&fd_table, old, &entry);
			if (rc == 0) {
				if (!drop_reference_if_disabled(old, entry))
					vector_decref(&fd_table, entry);
			}
		} else
			vector_decref(&fd_table, entry);
	}

	return newfd;
}

IOIL_PUBLIC FILE * IOIL_DECL(fdopen)(int fd, const char *mode)
{
	struct fd_entry *entry;
	int rc;

	IOIL_LOG_INFO("fdopen(%d, %s) intercepted", fd, mode);

	rc = vector_remove(&fd_table, fd, &entry);
	if (rc == 0) {
		IOIL_LOG_INFO("Removed IOF entry for fd=%d "
			      GAH_PRINT_STR, fd, GAH_PRINT_VAL(entry->gah));

		if (entry->pos != 0)
			__real_lseek(fd, entry->pos, SEEK_SET);

		vector_decref(&fd_table, entry);
	}

	return __real_fdopen(fd, mode);
}

IOIL_PUBLIC int IOIL_DECL(fcntl)(int fd, int cmd, ...)
{
	va_list ap;
	void *arg;
	struct fd_entry *entry = NULL;
	int rc;
	int newfd = -1;
	int fdarg;

	va_start(ap, cmd);
	arg = va_arg(ap, void *);
	va_end(ap);

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_fcntl(fd, cmd, arg);

	if (cmd == F_SETFL) { /* We don't support this flag for interception */
		entry->disabled = true;
		IOIL_LOG_INFO("Removed IOF entry for fd=%d " GAH_PRINT_STR ": "
			      "F_SETFL not supported for interception",
			      fd, GAH_PRINT_VAL(entry->gah));
		vector_remove(&fd_table, fd, NULL);
		vector_decref(&fd_table, entry);
		return __real_fcntl(fd, cmd, arg);
	}

	vector_decref(&fd_table, entry);

	if (cmd != F_DUPFD && cmd != F_DUPFD_CLOEXEC)
		return __real_fcntl(fd, cmd, arg);

	va_start(ap, cmd);
	fdarg = va_arg(ap, int);
	va_end(ap);
	newfd = __real_fcntl(fd, cmd, fdarg);

	if (newfd == -1)
		return newfd;

	/* Ok, newfd is a duplicate of fd */
	rc = vector_dup(&fd_table, fd, newfd, &entry);
	if (rc == 0 && entry != NULL) {
		IOIL_LOG_INFO("fcntl(%d, %d /* F_DUPFD* */, %d) intercepted "
			      GAH_PRINT_STR, fd, cmd, fdarg,
			      GAH_PRINT_VAL(entry->gah));
		if (drop_reference_if_disabled(newfd, entry)) {
			/* If the file was disabled, get the duplicated
			 * entry and if it hasn't changed, drop its
			 * reference too
			 */
			rc = vector_get(&fd_table, fd, &entry);
			if (rc == 0) {
				if (!drop_reference_if_disabled(fd, entry))
					vector_decref(&fd_table, entry);
			}
		} else
			vector_decref(&fd_table, entry);
	}

	return newfd;
}

FOREACH_ALIASED_INTERCEPT(IOIL_DECLARE_ALIAS)
