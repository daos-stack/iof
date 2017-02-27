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
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include "iof_ioctl.h"

int gah_test(const char *fname)
{
	struct ios_gah gah;
	int fd;
	int rc;
	int retcode = 0;

	fd = open(fname, O_RDONLY);

	if (fd == -1) {
		printf("ERROR: Failed to open file for test: %s\n", fname);
		return 1;
	}

	rc = ioctl(fd, IOF_IOCTL_GAH, &gah);

	if (rc != 0) {
		printf("ERROR: Failed ioctl test of IOF file: %s : %s\n",
		       fname, strerror(errno));
		retcode = rc;
	} else
		printf("ioctl returned " GAH_PRINT_STR "\n",
		       GAH_PRINT_VAL(gah));

	/* Run ioctl test on stdout.  Should fail */
	rc = ioctl(1, IOF_IOCTL_GAH, &gah);

	if (rc == 0) {
		printf("ERROR: Failed ioctl test of non-IOF file: %s\n", fname);
		retcode = 1;
	}

	return retcode;
}

#define IOF_MAX_PATH 4096
int main(int argc, char **argv)
{
	char buf[IOF_MAX_PATH];
	char *pos;
	FILE *fp;
	ssize_t last;
	int mnt_num = 0;
	int rc = 0;

	if (argc != 2) {
		printf("Usage: %s <cnss_prefix>\n", argv[0]);
		rc = 1;
		goto cleanup;
	}

	for (;;) {
		snprintf(buf, IOF_MAX_PATH,
			 "%s/.ctrl/iof/projections/%d/mount_point",
			 argv[1], mnt_num++);
		buf[IOF_MAX_PATH - 1] = 0;

		fp = fopen(buf, "r");
		if (fp == NULL) {
			printf("ERROR: No writeable mount found\n");
			rc = 1;
			goto cleanup;
		}
		fread(buf, IOF_MAX_PATH, 1, fp);
		last = ftell(fp);
		fclose(fp);

		buf[last] = 0;
		pos = strchr(buf, '\n');
		if (pos != NULL)
			last = (uintptr_t)pos - (uintptr_t)buf;
		else
			pos = buf + last;
		last += snprintf(pos, IOF_MAX_PATH - last, "/ioil_test_file");

		fp = fopen(buf, "w");
		if (fp == NULL) {
			printf("Skipping PA mount.  Can't write %s\n",
			       buf);
			continue;
		}

		fclose(fp);
		break;
	}

	rc = gah_test(buf);

cleanup:
	return rc;
}
