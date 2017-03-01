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
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <utime.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "log.h"
#include "ctrl_common.h"

int cnss_shutdown(void *arg)
{
	IOF_LOG_INFO("shutdown");
	return 0;
}

int cnss_client_attach(int client_id, void *arg)
{
	IOF_LOG_INFO("attached %d", client_id);
	return 0;
}

int cnss_client_detach(int client_id, void *arg)
{
	IOF_LOG_INFO("detached %d", client_id);
	return 0;
}

static int read_foo(char *buf, size_t len, void *arg)
{
	int *foo = (int *)arg;

	snprintf(buf, len, "%d", *foo);

	return 0;
}

static int write_foo(const char *value, void *arg)
{
	int val = atoi(value);
	int *foo = (int *)arg;

	*foo += val;

	return 0;
}

static int check_destroy_foo(void *arg)
{
	int *foo = (int *)arg;

	*foo = -1;

	return 0;
}


static int check_file_read(const char *prefix, const char *fname,
			   const char *expected, const char *source,
			   int line)
{
	char buf[256]; /* all of our values are less than 256 */
	int bytes;
	int fd;

	IOF_LOG_INFO("Run check at %s:%d\n", source, line);

	sprintf(buf, "%s%s", prefix, fname);

	fd = open(buf, O_RDONLY);

	if (fd == -1) {
		if (expected == NULL)
			return 0; /* Expected a failure */
		printf("Could not open %s.  Test %s:%d failed\n", fname,
		       source, line);
		return 1;
	}

	if (expected == NULL) {
		close(fd);
		printf("Should not be able to open %s.  Test %s:%d failed\n",
		       fname, source, line);
		return 1;
	}

	bytes = read(fd, buf, 256);

	close(fd);

	if (bytes == -1) {
		printf("Error reading %s.  Test %s:%d failed\n", fname, source,
		       line);
		return 1;
	}

	buf[bytes] = 0;

	IOF_LOG_INFO("Finished reading %s", buf);
	IOF_LOG_INFO("Comparing to %s", expected);

	if (strncmp(buf, expected, 256) != 0) {
		printf("Value unexpected in %s: (%s != %s).  Test"
		       " %s:%d failed\n", fname, buf, expected, source, line);
		return 1;
	}

	IOF_LOG_INFO("Done with check at %s:%d\n", source, line);

	return 0;
}

static int check_file_write(const char *prefix, const char *fname,
			    const char *value, const char *source,
			    int line)
{
	char buf[256]; /* all of our values are less than 256 */
	int bytes;
	int fd;

	IOF_LOG_INFO("Run check at %s:%d\n", source, line);

	sprintf(buf, "%s%s", prefix, fname);

	fd = open(buf, O_WRONLY|O_TRUNC);

	if (fd == -1) {
		if (value == NULL)
			return 0; /* Expected a failure */
		printf("Could not open %s.  Test %s:%d failed\n", fname,
		       source, line);
		return 1;
	}

	if (value == NULL) {
		close(fd);
		printf("Should not be able to open %s.  Test %s:%d failed\n",
		       fname, source, line);
		return 1;
	}

	bytes = write(fd, value, strlen(value));

	close(fd);

	if (bytes == -1) {
		printf("Error writing %s.  Test %s:%d failed\n", fname, source,
		       line);
		return 1;
	}

	if (bytes != strlen(value)) {
		printf("Error writing %s to %s.  Test %s:%d failed\n", value,
		       fname, source, line);
		return 1;
	}

	IOF_LOG_INFO("Done with check at %s:%d\n", source, line);

	return 0;
}

#define CHECK_FILE_READ(prefix, name, expected) \
	check_file_read(prefix, name, expected, __FILE__, __LINE__)

#define CHECK_FILE_WRITE(prefix, name, value) \
	check_file_write(prefix, name, value, __FILE__, __LINE__)

/* Test that large values get truncated properly */
#define TOO_LARGE 8192
static char large_constant[TOO_LARGE];

static int run_tests(const char *ctrl_prefix)
{
	int num_failures = 0;

	/* Only checks the first 256 bytes so this check will work */
	num_failures += CHECK_FILE_READ(ctrl_prefix, "/large",
					large_constant);
	num_failures += CHECK_FILE_READ(ctrl_prefix, "/class/bar/hello",
					"Hello World\n");
	num_failures += CHECK_FILE_READ(ctrl_prefix, "/class/bar/foo", "0\n");
	num_failures += CHECK_FILE_WRITE(ctrl_prefix, "/class/bar/foo", "10");
	num_failures += CHECK_FILE_READ(ctrl_prefix, "/class/bar/foo", "10\n");
	num_failures += CHECK_FILE_WRITE(ctrl_prefix, "/class/bar/foo", "55");
	num_failures += CHECK_FILE_READ(ctrl_prefix, "/class/bar/foo", "65\n");
	num_failures += CHECK_FILE_WRITE(ctrl_prefix, "/class/bar/foo", "-12");
	num_failures += CHECK_FILE_READ(ctrl_prefix, "/class/bar/foo", "53\n");
	num_failures += CHECK_FILE_READ(ctrl_prefix, "/client", "1\n");
	num_failures += CHECK_FILE_READ(ctrl_prefix, "/client", "2\n");
	num_failures += CHECK_FILE_READ(ctrl_prefix, "/client", "3\n");
	num_failures += CHECK_FILE_READ(ctrl_prefix, "/client", "4\n");

	return num_failures;
}

static int track_open(int *value, void *cb_arg)
{
	int *current = (int *)cb_arg;

	(*current)++;
	*value = *current;

	return 0;
}

static int track_close(int value, void *cb_arg)
{
	int *current = (int *)cb_arg;

	if (value != *current) {
		printf("Unexpected value for tracker %d\n", value);
		/* Changing this will cause the test to fail */
		(*current)++;
	}

	return 0;
}

int main(int argc, char **argv)
{
	char *prefix;
	char buf[32];
	char cmd_buf[32];
	char *end;
	int foo = 0;
	int opt;
	int tracker_value = 0;
	int num_failures;
	bool interactive = false;
	struct ctrl_dir *class_dir;
	struct ctrl_dir *bar_dir;

	memset(large_constant, 'a', TOO_LARGE);

	for (;;) {
		opt = getopt(argc, argv, "i");
		if (opt == -1)
			break;
		switch (opt) {
		case 'i':
			interactive = true;
			break;
		default:
			printf("Usage: %s [-i]\n", argv[0]);
			printf("\n    -i  Interactive run\n");
			return -1;
		}
	}

	strcpy(buf, "/tmp/iofXXXXXX");

	prefix = mkdtemp(buf);

	if (prefix == NULL) {
		printf("Could not allocate temporary directory for tests\n");
		return -1;
	}

	end = buf + strlen(buf);

	printf("Testing ctrl_fs in %s\n", buf);
	strcpy(end, "/iof.log");

	setenv("CRT_LOG_FILE", buf, 1);
	setenv("CRT_LOG_MASK", "INFO,ctrl=DEBUG", 1);
	iof_log_init("ctrl", "ctrl_fs_test");

	strcpy(end, "/.ctrl");

	ctrl_fs_start(buf);

	register_cnss_controls(3, NULL);

	ctrl_create_subdir(NULL, "class", &class_dir);
	ctrl_create_subdir(class_dir, "bar", &bar_dir);
	ctrl_register_variable(bar_dir, "foo", read_foo,
			       write_foo, check_destroy_foo, &foo);
	ctrl_register_constant(NULL, "large", large_constant);
	ctrl_register_constant(bar_dir, "hello", "Hello World");

	ctrl_register_tracker(NULL, "client", track_open, track_close,
			      check_destroy_foo, &tracker_value);

	num_failures = run_tests(buf);
	if (!interactive) { /* Invoke shutdown */
		strcpy(end, "/.ctrl/shutdown");
		utime(buf, NULL);
	}

	ctrl_fs_wait();

	if (foo != -1) {
		num_failures++;
		printf("Destroy callback never invoked\n");
	}
	if (tracker_value != -1) {
		num_failures++;
		printf("Tracker destroy callback never invoked\n");
	}
	if (num_failures != 0)
		printf("%d ctrl_fs tests failed\n", num_failures);
	else
		printf("All ctrl_fs tests passed\n");


	iof_log_close();

	if (!interactive) { /* Delete the temporary directory */
		*end = 0;
		sprintf(cmd_buf, "rm -rf %s", prefix);
		system(cmd_buf);
	}

	return num_failures;
}
