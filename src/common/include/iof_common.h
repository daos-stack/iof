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
#ifndef IOF_COMMON_H
#define IOF_COMMON_H
#include <sys/stat.h>
#include <crt_api.h>
#include <crt_util/common.h>
#include <string.h>

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

#define IOF_NAME_LEN_MAX 256
#define IOF_PREFIX_MAX 80
#define IOF_MAX_PATH_LEN 4096

/* IOF Registration op codes for RPC's*/

#define GETATTR_OP  (0xA1)
#define QUERY_PSR_OP (0x100)
#define SHUTDOWN_OP (0x200)

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

struct iof_getattr_out {
	crt_iov_t stat;
	uint64_t err;
};

struct psr_in {
	char *str;
};

/*input/output format for RPC's*/

struct crt_msg_field *string_in[] = {
	&CMF_STRING,
	&CMF_UINT64
};

struct crt_msg_field *getattr_out[] = {
	&CMF_IOVEC,
	&CMF_UINT64
};

struct crt_msg_field *psr_query[] = {
	&CMF_IOVEC
};

struct crt_msg_field *psr_query_in[] = {
	&CMF_UINT64
};

/*query RPC format*/
struct crt_req_format QUERY_RPC_FMT = DEFINE_CRT_REQ_FMT("psr_query",
							psr_query_in,
							psr_query);

/*getattr*/

struct crt_req_format GETATTR_FMT = DEFINE_CRT_REQ_FMT("getattr",
							string_in,
							getattr_out);

#endif
