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
#include "iof_ctrl_util.h"

FOREACH_INTERCEPT(IOIL_FORWARD_DECL)

bool ioil_initialized;
static __thread int saved_errno;
static vector_t fd_table;
static const char *cnss_prefix;
static crt_context_t crt_ctx;
static int cnss_id;
static struct iof_service_group *ionss_grps;
static uint32_t ionss_count;
static struct iof_projection *projections;
static uint32_t projection_count;

#define BLOCK_SIZE 1024

#define SAVE_ERRNO(is_error)                 \
	do {                                 \
		if (is_error)                \
			saved_errno = errno; \
	} while (0)

#define RESTORE_ERRNO(is_error)              \
	do {                                 \
		if (is_error)                \
			errno = saved_errno; \
	} while (0)

struct fd_entry {
	struct iof_file_common common;
	off_t pos;
	int flags;
	bool disabled;
};

int ioil_initialize_fd_table(int max_fds)
{
	int rc;

	rc = vector_init(&fd_table, sizeof(struct fd_entry), max_fds);

	if (rc != 0)
		IOF_LOG_ERROR("Could not allocate file descriptor table"
			      ", disabling kernel bypass: rc = %d", rc);
	return rc;
}

#define BUFSIZE 64

static int find_projections(void)
{
	char buf[IOF_CTRL_MAX_LEN];
	char tmp[BUFSIZE];
	int rc;
	int i;
	uint32_t version;
	crt_rank_t rank;
	uint32_t tag;

	rc = iof_ctrl_read_uint32(&version, "iof/ioctl_version");
	if (rc != 0) {
		IOF_LOG_ERROR("Could not read ioctl version, rc = %d", rc);
		return 1;
	}

	if (version != IOF_IOCTL_VERSION) {
		IOF_LOG_ERROR("IOCTL version mismatch: %d != %d", version,
			      IOF_IOCTL_VERSION);
		return 1;
	}

	rc = crt_group_config_path_set(cnss_prefix);
	if (rc != 0) {
		IOF_LOG_INFO("Could not set group config path, rc = %d", rc);
		return 1;
	}

	rc = iof_ctrl_read_uint32(&ionss_count, "iof/ionss_count");
	if (rc != 0) {
		IOF_LOG_INFO("Could not get ionss count, rc = %d", rc);
		return 1;
	}

	ionss_grps = calloc(ionss_count, sizeof(*ionss_grps));
	for (i = 0; i < ionss_count; i++) {
		struct iof_service_group *grp_info = &ionss_grps[i];

		snprintf(tmp, BUFSIZE, "iof/ionss/%d/name", i);

		rc = iof_ctrl_read_str(buf, IOF_CTRL_MAX_LEN, tmp);
		if (rc != 0) {
			IOF_LOG_INFO("Could not get ionss name, rc = %d", rc);
			return 1;
		}

		/* Ok, now try to attach.  Note, this will change when we
		 * attach to multiple IONSS processes
		 */
		rc = crt_group_attach(buf, &grp_info->dest_grp);
		if (rc != 0) {
			IOF_LOG_INFO("Could not attach to ionss %s, rc = %d",
				     buf, rc);
			return 1;
		}

		snprintf(tmp, BUFSIZE, "iof/ionss/%d/psr_rank", i);
		rc = iof_ctrl_read_uint32(&rank, tmp);
		if (rc != 0) {
			IOF_LOG_ERROR("Could not read psr_rank, rc = %d", rc);
			return 1;
		}

		grp_info->psr_ep.ep_rank = rank;
		grp_info->grp_id = i;

		snprintf(tmp, BUFSIZE, "iof/ionss/%d/psr_tag", i);
		rc = iof_ctrl_read_uint32(&tag, tmp);
		if (rc != 0) {
			IOF_LOG_ERROR("Could not read psr_tag, rc = %d", rc);
			return 1;
		}
		grp_info->psr_ep.ep_tag = tag;

		grp_info->enabled = true;
	}

	rc = iof_ctrl_read_uint32(&projection_count, "iof/projection_count");
	if (rc != 0) {
		IOF_LOG_ERROR("Could not read projection count, rc = %d", rc);
		return 1;
	}

	projections = calloc(projection_count, sizeof(*projections));
	if (projections == NULL) {
		IOF_LOG_ERROR("Could not allocate memory");
		return 1;
	}

	for (i = 0; i < projection_count; i++) {
		struct iof_projection *proj = &projections[i];

		proj->cli_fs_id = i;
		proj->crt_ctx = crt_ctx;
		snprintf(tmp, BUFSIZE, "iof/projections/%d/group_id", i);
		rc = iof_ctrl_read_uint32(&proj->grp_id, tmp);
		if (rc != 0) {
			IOF_LOG_ERROR("Could not read grp_id, rc = %d", rc);
			return 1;
		}

		if (proj->grp_id > ionss_count) {
			IOF_LOG_ERROR("Invalid grp_id for projection");
			return 1;
		}

		proj->grp = &ionss_grps[proj->grp_id];
		proj->enabled = true;
	}

	return 0;
}

static ssize_t pread_rpc(struct fd_entry *entry, char *buff, size_t len,
			 off_t offset)
{
	ssize_t bytes_read;
	int errcode;

	/* Just get rpc working then work out how to really do this */
	bytes_read = ioil_do_pread(buff, len, offset, &entry->common, &errcode);
	if (bytes_read < 0)
		saved_errno = errcode;
	return bytes_read;
}

/* Start simple and just loop */
static ssize_t preadv_rpc(struct fd_entry *entry, const struct iovec *iov,
			  int count, off_t offset)
{
	ssize_t bytes_read;
	int errcode;

	/* Just get rpc working then work out how to really do this */
	bytes_read = ioil_do_preadv(iov, count, offset, &entry->common,
				    &errcode);
	if (bytes_read < 0)
		saved_errno = errcode;
	return bytes_read;
}

static ssize_t pwrite_rpc(struct fd_entry *entry, const char *buff, size_t len,
			  off_t offset)
{
	ssize_t bytes_written;
	int errcode;

	/* Just get rpc working then work out how to really do this */
	bytes_written = ioil_do_pwrite(buff, len, offset, &entry->common,
				       &errcode);
	if (bytes_written < 0)
		saved_errno = errcode;

	return bytes_written;
}

/* Start simple and just loop */
static ssize_t pwritev_rpc(struct fd_entry *entry, const struct iovec *iov,
			   int count, off_t offset)
{
	ssize_t bytes_written;
	int errcode;

	/* Just get rpc working then work out how to really do this */
	bytes_written = ioil_do_pwritev(iov, count, offset, &entry->common,
					&errcode);
	if (bytes_written < 0)
		saved_errno = errcode;

	return bytes_written;
}

static __attribute__((constructor)) void ioil_init(void)
{
	char buf[IOF_CTRL_MAX_LEN];
	struct rlimit rlimit;
	int rc;

	FOREACH_INTERCEPT(IOIL_FORWARD_MAP_OR_FAIL);

	iof_log_init("IL", "IOIL", NULL);

	/* Get maximum number of file desciptors */
	rc = getrlimit(RLIMIT_NOFILE, &rlimit);
	if (rc != 0) {
		IOF_LOG_ERROR("Could not get process file descriptor limit"
			      ", disabling kernel bypass");
		return;
	}

	rc = ioil_initialize_fd_table(rlimit.rlim_max);
	if (rc != 0) {
		IOF_LOG_ERROR("Could not create fd_table, rc = %d,"
			      ", disabling kernel bypass", rc);
		return;
	}

	rc = iof_ctrl_util_init(&cnss_prefix, &cnss_id);

	if (rc != 0) {
		IOF_LOG_ERROR("Could not find CNSS (rc = %d)."
			      " disabling kernel bypass", rc);
		return;
	}

	rc = iof_ctrl_read_str(buf, IOF_CTRL_MAX_LEN, "crt_protocol");
	if (rc == 0)
		setenv("CRT_PHY_ADDR_STR", buf, 1);

	rc = crt_init(NULL, CRT_FLAG_BIT_SINGLETON);
	if (rc != 0) {
		IOF_LOG_ERROR("Could not initialize crt, rc = %d,"
			      " disabling kernel bypass", rc);
		return;
	}

	rc = crt_context_create(NULL, &crt_ctx);
	if (rc != 0) {
		IOF_LOG_ERROR("Could not create crt context, rc = %d,"
			      " disabling kernel bypass", rc);
		crt_finalize();
		return;
	}

	rc = iof_register(DEF_PROTO_CLASS(DEFAULT), NULL);
	if (rc != 0) {
		crt_context_destroy(crt_ctx, 0);
		crt_finalize();
		IOF_LOG_ERROR("Could not create crt context, rc = %d,"
			      " disabling kernel bypass", rc);
		return;
	}

	rc = find_projections();
	if (rc != 0) {
		IOF_LOG_ERROR("Could not configure projections, rc = %d"
			      " disabling kernel bypass", rc);
		iof_ctrl_util_finalize();
		return;
	}

	IOF_LOG_INFO("Using IONSS: cnss_prefix at %s, cnss_id is %d",
		     cnss_prefix, cnss_id);

	__sync_synchronize();

	ioil_initialized = true;
}

static __attribute__((destructor)) void ioil_fini(void)
{
	int i;

	if (ioil_initialized) {
		for (i = 0; i < ionss_count; i++)
			crt_group_detach(ionss_grps[i].dest_grp);
		crt_context_destroy(crt_ctx, 0);
		crt_finalize();
		iof_ctrl_util_finalize();
		free(ionss_grps);
		free(projections);
	}
	ioil_initialized = false;

	__sync_synchronize();

	iof_log_close();

	vector_destroy(&fd_table);
}

static void check_ioctl_on_open(int fd, int flags)
{
	struct fd_entry entry;
	struct iof_gah_info gah_info;
	int rc;

	if (fd == -1)
		return;

	rc = ioctl(fd, IOF_IOCTL_GAH, &gah_info);
	if (rc == 0) {
		IOIL_LOG_INFO("Opened an IOF file (fd = %d) "
			      GAH_PRINT_STR, fd, GAH_PRINT_VAL(gah_info.gah));
		if (gah_info.version != IOF_IOCTL_VERSION) {
			IOIL_LOG_INFO("IOF ioctl version mismatch: expected %d"
				      " actual %d", IOF_IOCTL_VERSION,
				      gah_info.version);
			return;
		}
		if (gah_info.cnss_id != cnss_id) {
			IOIL_LOG_INFO("IOF ioctl received from another CNSS: "
				      "expected %d actual %d", cnss_id,
				      gah_info.cnss_id);
			return;
		}
		entry.common.gah = gah_info.gah;
		entry.common.projection = &projections[gah_info.cli_fs_id];
		entry.common.gah_valid = true;
		entry.common.ep = entry.common.projection->grp->psr_ep;
		entry.pos = 0;
		entry.flags = flags;
		entry.disabled = false;
		rc = vector_set(&fd_table, fd, &entry);
		if (rc != 0)
			IOIL_LOG_INFO("Failed to insert gah in table, rc = %d",
				     rc);
	}
}

static bool drop_reference_if_disabled(int fd, struct fd_entry *entry)
{
	if (!entry->disabled)
		return false;

	IOIL_LOG_INFO("Dropped reference to disabled file " GAH_PRINT_STR,
		     GAH_PRINT_VAL(entry->common.gah));
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

	SAVE_ERRNO(fd == -1);

	/* Ignore O_APPEND files for now */
	if (ioil_initialized && ((flags & (O_PATH|O_APPEND)) == 0))
		check_ioctl_on_open(fd, flags);

	RESTORE_ERRNO(fd == -1);

	return fd;
}

IOIL_PUBLIC int IOIL_DECL(creat)(const char *pathname, mode_t mode)
{
	int fd;

	IOIL_LOG_INFO("creat(%s, 0%o) intercepted", pathname, mode);

	/* Same as open with O_CREAT|O_WRONLY|O_TRUNC */
	fd = __real_open(pathname, O_CREAT|O_WRONLY|O_TRUNC, mode);

	SAVE_ERRNO(fd == -1);

	if (ioil_initialized)
		check_ioctl_on_open(fd, O_CREAT|O_WRONLY|O_TRUNC);

	RESTORE_ERRNO(fd == -1);

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
			      GAH_PRINT_STR, fd,
			      GAH_PRINT_VAL(entry->common.gah));
		vector_decref(&fd_table, entry);
	}

	return __real_close(fd);
}

IOIL_PUBLIC ssize_t IOIL_DECL(read)(int fd, void *buf, size_t len)
{
	struct fd_entry *entry;
	ssize_t bytes_read;
	off_t oldpos;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_read(fd, buf, len);

	IOIL_LOG_INFO("read(%d, %p, %zu) intercepted " GAH_PRINT_STR,
		      fd, buf, len, GAH_PRINT_VAL(entry->common.gah));

	oldpos = entry->pos;
	bytes_read = pread_rpc(entry, buf, len, oldpos);
	if (bytes_read > 0)
		entry->pos = oldpos + bytes_read;
	vector_decref(&fd_table, entry);

	RESTORE_ERRNO(bytes_read < 0);

	return bytes_read;
}

IOIL_PUBLIC ssize_t IOIL_DECL(pread)(int fd, void *buf,
				     size_t len, off_t offset)
{
	struct fd_entry *entry;
	ssize_t bytes_read;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_pread(fd, buf, len, offset);

	IOIL_LOG_INFO("pread(%d, %p, %zu, %zd) intercepted " GAH_PRINT_STR, fd,
		      buf, len, offset, GAH_PRINT_VAL(entry->common.gah));

	bytes_read = pread_rpc(entry, buf, len, offset);

	vector_decref(&fd_table, entry);

	RESTORE_ERRNO(bytes_read < 0);

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
		      fd, buf, len, GAH_PRINT_VAL(entry->common.gah));

	oldpos = entry->pos;
	bytes_written = pwrite_rpc(entry, buf, len, entry->pos);
	if (bytes_written > 0)
		entry->pos = oldpos + bytes_written;
	vector_decref(&fd_table, entry);

	RESTORE_ERRNO(bytes_written < 0);

	return bytes_written;
}

IOIL_PUBLIC ssize_t IOIL_DECL(pwrite)(int fd, const void *buf,
				      size_t len, off_t offset)
{
	struct fd_entry *entry;
	ssize_t bytes_written;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_pwrite(fd, buf, len, offset);

	IOIL_LOG_INFO("pwrite(%d, %p, %zu, %zd) intercepted " GAH_PRINT_STR, fd,
		      buf, len, offset, GAH_PRINT_VAL(entry->common.gah));

	bytes_written = pwrite_rpc(entry, buf, len, offset);

	vector_decref(&fd_table, entry);

	RESTORE_ERRNO(bytes_written < 0);

	return bytes_written;
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
		      offset, whence, GAH_PRINT_VAL(entry->common.gah));

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

	SAVE_ERRNO(new_offset < 0);

	vector_decref(&fd_table, entry);

	RESTORE_ERRNO(new_offset < 0);

	return new_offset;
}

IOIL_PUBLIC ssize_t IOIL_DECL(readv)(int fd, const struct iovec *vector,
				     int count)
{
	struct fd_entry *entry;
	ssize_t bytes_read;
	off_t oldpos;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_readv(fd, vector, count);

	IOIL_LOG_INFO("readv(%d, %p, %d) intercepted " GAH_PRINT_STR,
		      fd, vector, count, GAH_PRINT_VAL(entry->common.gah));

	oldpos = entry->pos;
	bytes_read = preadv_rpc(entry, vector, count, entry->pos);
	if (bytes_read > 0)
		entry->pos = oldpos + bytes_read;
	vector_decref(&fd_table, entry);

	RESTORE_ERRNO(bytes_read < 0);

	return bytes_read;
}

IOIL_PUBLIC ssize_t IOIL_DECL(preadv)(int fd, const struct iovec *vector,
				      int count, off_t offset)
{
	struct fd_entry *entry;
	ssize_t bytes_read;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_preadv(fd, vector, count, offset);

	IOIL_LOG_INFO("preadv(%d, %p, %d, %zd) intercepted " GAH_PRINT_STR, fd,
		      vector, count, offset, GAH_PRINT_VAL(entry->common.gah));

	bytes_read = preadv_rpc(entry, vector, count, offset);
	vector_decref(&fd_table, entry);

	RESTORE_ERRNO(bytes_read < 0);

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
		      fd, vector, count, GAH_PRINT_VAL(entry->common.gah));

	oldpos = entry->pos;
	bytes_written = pwritev_rpc(entry, vector, count, entry->pos);
	if (bytes_written > 0)
		entry->pos = oldpos + bytes_written;
	vector_decref(&fd_table, entry);

	RESTORE_ERRNO(bytes_written < 0);

	return bytes_written;
}

IOIL_PUBLIC ssize_t IOIL_DECL(pwritev)(int fd, const struct iovec *vector,
					 int count, off_t offset)
{
	struct fd_entry *entry;
	ssize_t bytes_written;
	int rc;

	rc = vector_get(&fd_table, fd, &entry);
	if (rc != 0 || drop_reference_if_disabled(fd, entry))
		return __real_pwritev(fd, vector, count, offset);

	IOIL_LOG_INFO("pwritev(%d, %p, %d, %zd) intercepted " GAH_PRINT_STR, fd,
		      vector, count, offset, GAH_PRINT_VAL(entry->common.gah));

	bytes_written = pwritev_rpc(entry, vector, count, offset);

	vector_decref(&fd_table, entry);

	RESTORE_ERRNO(bytes_written < 0);

	return bytes_written;
}

IOIL_PUBLIC void *IOIL_DECL(mmap)(void *address, size_t length, int protect,
				    int flags, int fd, off_t offset)
{
	struct fd_entry *entry;
	int rc;

	rc = vector_remove(&fd_table, fd, &entry);
	if (rc == 0) {
		IOIL_LOG_INFO("mmap(%p, %zu, %d, %d, %d, %zd) intercepted, "
			      "disabling kernel bypass " GAH_PRINT_STR, address,
			      length, protect, flags, fd, offset,
			      GAH_PRINT_VAL(entry->common.gah));

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
		      GAH_PRINT_VAL(entry->common.gah));
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
		      fd, GAH_PRINT_VAL(entry->common.gah));
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
			      fd, GAH_PRINT_VAL(entry->common.gah));
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
			      old, new, GAH_PRINT_VAL(entry->common.gah));
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
			      GAH_PRINT_STR, fd,
			      GAH_PRINT_VAL(entry->common.gah));

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
			      "F_SETFL not supported for kernel bypass",
			      fd, GAH_PRINT_VAL(entry->common.gah));
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
			      GAH_PRINT_VAL(entry->common.gah));
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
