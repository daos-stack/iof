/* Copyright (C) 2016 Intel Corporation
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
#include "version.h"
#include "log.h"
#include "iof_common.h"

static  struct iof_fs_info *fs_list;
static int num_fs;
static int shutdown;

int shutdown_handler(crt_rpc_t *rpc_req)
{
	int		ret;

	IOF_LOG_DEBUG("Shutdown request recieved\n");
	ret = crt_reply_send(rpc_req);
	if (ret)
		IOF_LOG_ERROR("could not send shutdown reply");

	shutdown = 1;

	return ret;
}

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

int iof_getattr_handler(crt_rpc_t *getattr_rpc)
{
	struct iof_string_in *in = NULL;
	struct iof_getattr_out *out = NULL;
	uint64_t ret;
	char new_path[IOF_MAX_PATH_LEN];
	struct stat stbuf = {0};

	in = crt_req_get(getattr_rpc);
	if (in == NULL) {
		IOF_LOG_ERROR("Could not retrieve input args");
		return 0;
	}

	out = crt_reply_get(getattr_rpc);
	if (out == NULL) {
		IOF_LOG_ERROR("Could not retrieve output args");
		return 0;
	}

	IOF_LOG_DEBUG("Checking path %s", in->path);
	ret = (uint64_t)iof_get_path(in->my_fs_id, in->path, &new_path[0]);
	if (ret) {
		IOF_LOG_ERROR("could not construct filesystem path, ret = %lu",
				ret);
		out->err = ret;
	} else {
		int rc;

		out->err = 0;
		errno = 0;
		rc = lstat(new_path, &stbuf);
		if (rc == 0) {
			out->rc = 0;
			crt_iov_set(&out->stat, &stbuf, sizeof(struct stat));
		} else {
			out->rc = errno;
		}
	}

	IOF_LOG_DEBUG("path %s result err %d rc %d",
		      in->path, out->err, out->rc);

	ret = crt_reply_send(getattr_rpc);
	if (ret)
		IOF_LOG_ERROR("getattr: response not sent, ret = %lu", ret);
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

	ret = crt_rpc_srv_register(GETATTR_OP, &GETATTR_FMT,
			iof_getattr_handler);
	if (ret) {
		IOF_LOG_ERROR("Can not register getattr RPC, ret = %d", ret);
		return ret;
	}

	ret = crt_rpc_srv_register(SHUTDOWN_OP, NULL, shutdown_handler);
	if (ret) {
		IOF_LOG_ERROR("Can not register shutdown RPC, ret = %d", ret);
		return ret;
	}


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

	for (i = 0; i < num_fs; i++) {
		char *full_path = realpath(argv[i+1], NULL);

		if (!full_path)
			continue;
		IOF_LOG_INFO("Projecting %s", full_path);
		fs_list[i].mode = 0;
		fs_list[i].id = i;
		strncpy(fs_list[i].mnt, full_path, IOF_NAME_LEN_MAX);
		free(full_path);
	}


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

cleanup:
	if (fs_list)
		free(fs_list);

	iof_log_close();

	return ret;
}
