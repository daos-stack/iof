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
#ifndef IOF_COMMON_H
#define IOF_COMMON_H
#include <sys/stat.h>
#include <crt_api.h>
#include <crt_util/common.h>

#include <ios_gah.h>

#define IOF_SUCCESS		0
#define IOF_ERR_MOUNT		1
#define IOF_ERR_NOMEM		2
#define IOF_ERR_PROJECTION	3
#define IOF_ERR_OVERFLOW	4
#define IOF_ERR_CART		5
#define IOF_BAD_DATA		6
#define IOF_NOT_SUPP		7 /*Not supported*/
#define	IOF_ERR_INTERNAL	8
#define IOF_ERR_PTHREAD		9
#define IOF_GAH_INVALID        10

#define IOF_NAME_LEN_MAX 256
#define IOF_PREFIX_MAX 80
#define IOF_MAX_PATH_LEN 4096

/* IOF Registration op codes for RPC's*/

#define QUERY_PSR_OP	(0x201)
#define SHUTDOWN_OP	(0x202)
#define READDIR_OP	(0x204)
#define CLOSEDIR_OP	(0x205)
#define OPEN_OP		(0x206)
#define CLOSE_OP	(0x207)
#define CREATE_OP	(0x208)
#define READ_OP		(0x209)
#define MKDIR_OP	(0x20B)

struct iof_fs_info {
	/*Associated mount point*/
	char mnt[IOF_NAME_LEN_MAX];
	/*id of filesystem*/
	uint64_t id;
	/*mode of projection, set to 0 for private mode*/
	uint8_t mode;
};

struct iof_psr_query {
	crt_iov_t query_list;
};

struct iof_string_in {
	crt_string_t path;
	uint64_t my_fs_id;
};

struct iof_create_in {
	crt_string_t path;
	uint64_t my_fs_id;
	mode_t mode;
};

struct iof_getattr_out {
	crt_iov_t stat;
	int rc;
	int err;
};

struct iof_opendir_out {
	struct ios_gah gah;
	int rc;
	int err;
};

struct iof_readdir_in {
	struct ios_gah gah;
	uint64_t my_fs_id;
	int offsef;
};

/* Each READDIR rpc contains an array of these */
struct iof_readdir_reply {
	char d_name[256];
	struct stat stat;
	int read_rc;
	int stat_rc;
	int last;
};

struct iof_readdir_out {
	crt_iov_t replies;
	int err;
};

struct iof_open_out {
	struct ios_gah gah;
	int rc;
	int err;
};

struct iof_closedir_in {
	struct ios_gah gah;
	uint64_t my_fs_id;
};

struct iof_read_in {
	struct ios_gah gah;
	uint64_t base;
	uint64_t len;
};

struct iof_data_out {
	crt_iov_t data;
	int rc;
	int err;
};

struct iof_status_out {
	int rc;
	int err;
};

extern struct crt_req_format READDIR_FMT;

extern struct crt_req_format QUERY_RPC_FMT;

extern struct crt_req_format CLOSEDIR_FMT;

extern struct crt_req_format OPEN_FMT;

extern struct crt_req_format CLOSE_FMT;

extern struct crt_req_format CREATE_FMT;

extern struct crt_req_format READ_FMT;

extern struct crt_req_format MKDIR_FMT;

struct rpc_data {
	struct crt_req_format fmt;
	void *fn;
	int op_id;
};

#define MY_TYPE(TYPE) struct rpc_data TYPE
struct my_types {
	MY_TYPE(getattr);
	MY_TYPE(opendir);
};

struct proto {
	char name[16];
	int id_base;
	struct my_types mt;
};

struct proto *iof_register();

int iof_proto_commit(struct proto *);

#endif
