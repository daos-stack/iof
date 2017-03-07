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
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <string.h>
#include "intercept.h"
#include "iof_ioctl.h"
#include "ios_gah.h"

IOIL_FORWARD_DECL(int, open, (const char *pathname, int flags, ...));
IOIL_FORWARD_DECL(int, open64, (const char *pathname, int flags, ...));
IOIL_FORWARD_DECL(int, close, (int fd));
IOIL_FORWARD_DECL(FILE *, fdopen, (int fd, const char *mode));

bool ioil_initialized;

static __attribute__((constructor)) void ioil_init(void)
{
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

int IOIL_DECL(open)(const char *pathname, int flags, ...)
{
	int fd;
	unsigned int mode; /* mode_t gets "promoted" to unsigned int
			    * for va_arg routine
			    */

	IOIL_FORWARD_MAP_OR_FAIL(open);

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

int IOIL_DECL(open64)(const char *pathname, int flags, ...)
{
	int fd;
	unsigned int mode; /* mode_t gets "promoted" to unsigned int
			    * for va_arg routine
			    */
	IOIL_FORWARD_MAP_OR_FAIL(open64);

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

int IOIL_DECL(creat)(const char *pathname, mode_t mode)
{
	IOIL_FORWARD_MAP_OR_FAIL(open);

	IOIL_LOG_INFO("creat(%s, 0%o) intercepted", pathname, mode);
	/* Same as open with O_CREAT|O_WRONLY|O_TRUNC */
	return __real_open(pathname, O_CREAT|O_WRONLY|O_TRUNC, mode);
}

int IOIL_DECL(creat64)(const char *pathname, mode_t mode)
{
	IOIL_FORWARD_MAP_OR_FAIL(open64);

	IOIL_LOG_INFO("creat64(%s, 0%o) intercepted", pathname, mode);
	/* Same as open with O_CREAT|O_WRONLY|O_TRUNC */
	return __real_open64(pathname, O_CREAT|O_WRONLY|O_TRUNC, mode);
}

int IOIL_DECL(close)(int fd)
{
	IOIL_FORWARD_MAP_OR_FAIL(close);

	IOIL_LOG_INFO("close(%d) intercepted", fd);

	return __real_close(fd);
}

FILE *IOIL_DECL(fdopen)(int fd, const char *mode)
{
	IOIL_FORWARD_MAP_OR_FAIL(fdopen);

	IOIL_LOG_INFO("fdopen(%d, %s) intercepted", fd, mode);

	return __real_fdopen(fd, mode);
}
