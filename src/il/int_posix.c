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
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <string.h>
#include "intercept.h"
#include "iof_ioctl.h"

FOREACH_INTERCEPT(IOIL_FORWARD_DECL);

bool ioil_initialized;

static __attribute__((constructor)) void ioil_init(void)
{
	FOREACH_INTERCEPT(IOIL_FORWARD_MAP_OR_FAIL);

	iof_log_init("IL", "IOIL");

	__sync_synchronize();

	ioil_initialized = true;
}

static __attribute__((destructor)) void ioil_fini(void)
{
	iof_log_close();
}

static void check_ioctl_on_open(int fd)
{
	struct ios_gah gah;
	int saved_errno;
	int rc;

	if (fd == -1)
		return;

	saved_errno = errno; /* Save the errno from open */

	rc = ioctl(fd, IOF_IOCTL_GAH, &gah);
	if (rc == -1)
		IOIL_LOG_INFO("opened non-IOF file, %s", strerror(errno));
	else
		IOIL_LOG_INFO("opened IOF file " GAH_PRINT_STR,
			     GAH_PRINT_VAL(gah));

	errno = saved_errno; /* Restore the errno from open */
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

	if (ioil_initialized)
		check_ioctl_on_open(fd);

	return fd;
}

IOIL_PUBLIC int IOIL_DECL(open64)(const char *pathname, int flags, ...)
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

		IOIL_LOG_INFO("open64(%s, 0%o, 0%o) intercepted",
			     pathname, flags, mode);

		fd = __real_open64(pathname, flags, mode);
	} else {
		IOIL_LOG_INFO("open64(%s, 0%o) intercepted", pathname, flags);

		fd =  __real_open64(pathname, flags);
	}

	if (ioil_initialized)
		check_ioctl_on_open(fd);

	return fd;
}

IOIL_PUBLIC int IOIL_DECL(creat)(const char *pathname, mode_t mode)
{
	IOIL_LOG_INFO("creat(%s, 0%o) intercepted", pathname, mode);

	/* Same as open with O_CREAT|O_WRONLY|O_TRUNC */
	return __real_open(pathname, O_CREAT|O_WRONLY|O_TRUNC, mode);
}

IOIL_PUBLIC int IOIL_DECL(creat64)(const char *pathname, mode_t mode)
{
	IOIL_LOG_INFO("creat64(%s, 0%o) intercepted", pathname, mode);

	/* Same as open with O_CREAT|O_WRONLY|O_TRUNC */
	return __real_open64(pathname, O_CREAT|O_WRONLY|O_TRUNC, mode);
}

IOIL_PUBLIC int IOIL_DECL(close)(int fd)
{
	IOIL_LOG_INFO("close(%d) intercepted", fd);

	return __real_close(fd);
}

IOIL_PUBLIC ssize_t IOIL_DECL(read)(int fd, void *buf, size_t len)
{
	IOIL_LOG_INFO("read(%d, %p, %zu) intercepted", fd, buf, len);

	return __real_read(fd, buf, len);
}

IOIL_PUBLIC ssize_t IOIL_DECL(pread)(int fd, void *buf,
				     size_t len, off_t offset)
{
	IOIL_LOG_INFO("pread(%d, %p, %zu, %zd) intercepted",
		     fd, buf, len, offset);

	return __real_pread(fd, buf, len, offset);
}

IOIL_PUBLIC ssize_t IOIL_DECL(pread64)(int fd, void *buf,
				       size_t len, off64_t offset)
{
	IOIL_LOG_INFO("pread64(%d, %p, %zu, %" PRId64 ") intercepted",
		     fd, buf, len, offset);

	return __real_pread64(fd, buf, len, offset);
}

IOIL_PUBLIC ssize_t IOIL_DECL(write)(int fd, const void *buf, size_t len)
{
	/* Logging here currently creates an infinite recursion on the
	 * cart log.   Eventually, we can only log when it's one of our
	 * files.  Turn it off for now.
	 * IOIL_LOG_INFO("write(%d, %p, %zu) intercepted", fd, buf, len);
	 */

	return __real_write(fd, buf, len);
}

IOIL_PUBLIC ssize_t IOIL_DECL(pwrite)(int fd, const void *buf,
				      size_t len, off_t offset)
{
	IOIL_LOG_INFO("pwrite(%d, %p, %zu, %zd) intercepted",
		     fd, buf, len, offset);

	return __real_pwrite(fd, buf, len, offset);
}

IOIL_PUBLIC ssize_t IOIL_DECL(pwrite64)(int fd, const void *buf,
					size_t len, off64_t offset)
{
	IOIL_LOG_INFO("pwrite64(%d, %p, %zu, %" PRId64 ") intercepted",
		     fd, buf, len, offset);

	return __real_pwrite64(fd, buf, len, offset);
}

IOIL_PUBLIC off_t IOIL_DECL(lseek)(int fd, off_t offset, int whence)
{
	IOIL_LOG_INFO("lseek(%d, %zd, %d) intercepted", fd, offset, whence);

	return __real_lseek(fd, offset, whence);
}

IOIL_PUBLIC off64_t IOIL_DECL(lseek64)(int fd, off64_t offset, int whence)
{
	IOIL_LOG_INFO("lseek64(%d, %" PRId64 ", %d) intercepted",
		     fd, offset, whence);

	return __real_lseek64(fd, offset, whence);
}

IOIL_PUBLIC ssize_t IOIL_DECL(readv)(int fd, const struct iovec *vector,
				     int count)
{
	IOIL_LOG_INFO("readv(%d, %p, %d) intercepted", fd, vector, count);

	return __real_readv(fd, vector, count);
}

IOIL_PUBLIC ssize_t IOIL_DECL(writev)(int fd, const struct iovec *vector,
				      int count)
{
	IOIL_LOG_INFO("writev(%d, %p, %d) intercepted", fd, vector, count);

	return __real_writev(fd, vector, count);
}

IOIL_PUBLIC void *IOIL_DECL(mmap)(void *address, size_t length, int protect,
				  int flags, int fd, off_t offset)
{
	IOIL_LOG_INFO("mmap(%p, %zu, %d, %d, %d, %zd) intercepted",
		     address, length, protect, flags, fd, offset);

	return __real_mmap(address, length, protect, flags, fd, offset);

}

IOIL_PUBLIC void *IOIL_DECL(mmap64)(void *address, size_t length, int protect,
				    int flags, int fd, off64_t offset)
{
	IOIL_LOG_INFO("mmap64(%p, %zu, %d, %d, %d, %" PRId64 ") intercepted",
		     address, length, protect, flags, fd, offset);

	return __real_mmap64(address, length, protect, flags, fd, offset);
}

IOIL_PUBLIC int IOIL_DECL(fsync)(int fd)
{
	IOIL_LOG_INFO("fsync(%d) intercepted", fd);

	return __real_fsync(fd);
}

IOIL_PUBLIC int IOIL_DECL(fdatasync)(int fd)
{
	IOIL_LOG_INFO("fdatasync(%d) intercepted", fd);

	return __real_fdatasync(fd);
}

IOIL_PUBLIC int IOIL_DECL(dup)(int fd)
{
	IOIL_LOG_INFO("dup(%d) intercepted", fd);

	return __real_dup(fd);
}

IOIL_PUBLIC int IOIL_DECL(dup2)(int old, int new)
{
	IOIL_LOG_INFO("dup2(%d, %d) intercepted", old, new);

	return __real_dup2(old, new);
}

IOIL_PUBLIC FILE * IOIL_DECL(fdopen)(int fd, const char *mode)
{
	IOIL_LOG_INFO("fdopen(%d, %s) intercepted", fd, mode);

	return __real_fdopen(fd, mode);
}
