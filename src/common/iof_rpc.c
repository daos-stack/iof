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

#include "iof_common.h"
#include "log.h"

/*input/output format for RPC's*/

/*
 * Re-use the CMF_UUID type when using a GAH as they are both 128 bit types
 * but define CMF_GAH here so it's clearer in the code what is happening.
 */
#define CMF_GAH CMF_UUID

int
iof_proc_name(crt_proc_t proc, void *arg)
{
	struct ios_name *data = arg;

	return crt_proc_memcpy(proc, data, sizeof(*data));
}

int
iof_proc_stat(crt_proc_t proc, void *arg)
{
	struct stat *data = arg;

	return crt_proc_memcpy(proc, data, sizeof(*data));
}

struct crt_msg_field CMF_IOF_NAME = {
	.cmf_size = sizeof(struct ios_name),
	.cmf_proc = iof_proc_name,
};

struct crt_msg_field CMF_IOF_STAT = {
	.cmf_size = sizeof(struct stat),
	.cmf_proc = iof_proc_stat,
};

struct crt_msg_field *gah_string_in[] = {
	&CMF_GAH,	/* gah */
	&CMF_IOF_NAME,	/* name */
};

struct crt_msg_field *string_in[] = {
	&CMF_STRING,
	&CMF_INT,
};

struct crt_msg_field *string_out[] = {
	&CMF_STRING,
	&CMF_INT,
	&CMF_INT,
};

struct crt_msg_field *entry_out[] = {
	&CMF_GAH,	/* gah */
	&CMF_IOF_STAT,	/* struct stat */
	&CMF_INT,	/* rc */
	&CMF_INT,	/* err */
};

struct crt_msg_field *create_out[] = {
	&CMF_GAH,	/* gah */
	&CMF_GAH,	/* inode gah */
	&CMF_IOF_STAT,	/* struct stat */
	&CMF_INT,	/* rc */
	&CMF_INT,	/* err */
};

struct crt_msg_field *two_string_in[] = {
	&CMF_GAH,
	&CMF_IOF_NAME,
	&CMF_STRING,
};

struct crt_msg_field *create_in[] = {
	&CMF_GAH,	/* gah */
	&CMF_IOF_NAME,	/* name */
	&CMF_INT,	/* mode */
	&CMF_INT,	/* flags */
};

struct crt_msg_field *rename_in[] = {
	&CMF_GAH,	/* old parent */
	&CMF_GAH,	/* new parent */
	&CMF_IOF_NAME,	/* old name */
	&CMF_IOF_NAME,	/* new name */
	&CMF_INT,	/* flags */
};

struct crt_msg_field *open_in[] = {
	&CMF_GAH,	/* gah */
	&CMF_INT,	/* flags */
};

struct crt_msg_field *unlink_in[] = {
	&CMF_IOF_NAME,	/* name */
	&CMF_GAH,	/* gah */
	&CMF_INT,	/* flags */
};

struct crt_msg_field *attr_out[] = {
	&CMF_IOF_STAT,	/* stat */
	&CMF_INT,	/* rc */
	&CMF_INT,	/* err */
};

struct crt_msg_field *iov_pair[] = {
	&CMF_IOVEC,
	&CMF_INT,
	&CMF_INT
};

struct crt_msg_field *gah_pair[] = {
	&CMF_GAH,
	&CMF_INT,
	&CMF_INT
};

struct crt_msg_field *readdir_in[] = {
	&CMF_GAH,
	&CMF_BULK,
	&CMF_UINT64,
	&CMF_INT,
};

struct crt_msg_field *readdir_out[] = {
	&CMF_IOVEC,
	&CMF_INT,
	&CMF_INT,
	&CMF_INT,
	&CMF_INT,
};

struct crt_msg_field *psr_out[] = {
	&CMF_UINT32,
	&CMF_UINT32,
	&CMF_IOVEC,
	&CMF_UINT32,
	&CMF_UINT32,
	&CMF_UINT32,
};

struct crt_msg_field *readx_in[] = {
	&CMF_GAH,	/* gah */
	&CMF_UINT64,	/* base */
	&CMF_UINT64,	/* len */
	&CMF_UINT64,	/* xtvec_len */
	&CMF_UINT64,	/* bulk_len */
	&CMF_BULK,	/* xtvec_bulk */
	&CMF_BULK,	/* data_bulk */
};

struct crt_msg_field *readx_out[] = {
	&CMF_IOVEC,	/* data */
	&CMF_UINT64,	/* bulk_len */
	&CMF_UINT32,	/* iov_len */
	&CMF_INT,	/* rc */
	&CMF_INT,	/* err */
};

struct crt_msg_field *truncate_in[] = {
	&CMF_STRING,
	&CMF_UINT64,
	&CMF_INT,
};

struct crt_msg_field *ftruncate_in[] = {
	&CMF_GAH,
	&CMF_UINT64
};

struct crt_msg_field *status_out[] = {
	&CMF_INT,
	&CMF_INT,
};

struct crt_msg_field *gah_in[] = {
	&CMF_GAH,
};

struct crt_msg_field *writex_in[] = {
	&CMF_GAH,
	&CMF_IOVEC,
	&CMF_UINT64,
	&CMF_UINT64,
	&CMF_UINT64,
	&CMF_UINT64,
	&CMF_BULK,
	&CMF_BULK,
};

struct crt_msg_field *writex_out[] = {
	&CMF_UINT64,
	&CMF_INT,
	&CMF_INT,
	&CMF_UINT64,
	&CMF_UINT64,
};

struct crt_msg_field *chmod_in[] = {
	&CMF_STRING,
	&CMF_INT,
	&CMF_INT,
};

struct crt_msg_field *chmod_gah_in[] = {
	&CMF_GAH,
	&CMF_INT,
};

struct crt_msg_field *utimens_in[] = {
	&CMF_STRING,
	&CMF_IOVEC,
	&CMF_INT,
};

struct crt_msg_field *utimens_gah_in[] = {
	&CMF_GAH,
	&CMF_IOVEC,
};

struct crt_msg_field *setattr_in[] = {
	&CMF_GAH,	/* gah */
	&CMF_IOF_STAT,	/* struct stat */
	&CMF_UINT32,	/* to_set */
};

/*query RPC format*/
struct crt_req_format QUERY_RPC_FMT = DEFINE_CRT_REQ_FMT("psr_query",
							 NULL,
							 psr_out);

#define RPC_TYPE(NAME, in, out) { .fmt = DEFINE_CRT_REQ_FMT(#NAME, in, out) }

struct rpc_data default_rpc_types[] = {
	RPC_TYPE(opendir, gah_in, gah_pair),
	RPC_TYPE(readdir, readdir_in, readdir_out),
	RPC_TYPE(closedir, gah_in, NULL),
	RPC_TYPE(getattr_gah, gah_in, attr_out),
	RPC_TYPE(writex, writex_in, writex_out),
	RPC_TYPE(truncate, truncate_in, status_out),
	RPC_TYPE(ftruncate, ftruncate_in, status_out),
	RPC_TYPE(rmdir, string_in, status_out),
	RPC_TYPE(rename, two_string_in, status_out),
	RPC_TYPE(rename_ll, rename_in, status_out),
	RPC_TYPE(readx, readx_in, readx_out),
	RPC_TYPE(unlink, unlink_in, status_out),
	RPC_TYPE(open, open_in, gah_pair),
	RPC_TYPE(create, create_in, create_out),
	RPC_TYPE(close, gah_in, NULL),
	RPC_TYPE(mkdir, create_in, status_out),
	RPC_TYPE(mkdir_ll, create_in, entry_out),
	RPC_TYPE(readlink, string_in, string_out),
	RPC_TYPE(readlink_ll, gah_in, string_out),
	RPC_TYPE(symlink, two_string_in, status_out),
	RPC_TYPE(symlink_ll, two_string_in, entry_out),
	RPC_TYPE(fsync, gah_in, status_out),
	RPC_TYPE(fdatasync, gah_in, status_out),
	RPC_TYPE(chmod, chmod_in, status_out),
	RPC_TYPE(chmod_gah, chmod_gah_in, status_out),
	RPC_TYPE(utimens, utimens_in, status_out),
	RPC_TYPE(utimens_gah, utimens_gah_in, status_out),
	RPC_TYPE(statfs, gah_in, iov_pair),
	RPC_TYPE(lookup, gah_string_in, entry_out),
	RPC_TYPE(setattr, setattr_in, attr_out),
};

const struct proto iof_protocol_registry[IOF_PROTO_CLASSES] = {
	[DEF_PROTO_CLASS(DEFAULT)] = {
		.name = "IOF_PRIVATE",
		.id_base = 0x10F00,
		.rpc_type_count = IOF_DEFAULT_RPC_TYPES,
		.rpc_types = default_rpc_types
	}
};

/* Bulk register a RPC type
 *
 * If there is a failure then register what is possible, and return
 * the first error that occurred.
 */
int iof_register(enum iof_proto_class cls, crt_rpc_cb_t handlers[])
{
	int i, ret = 0;
	const struct proto *proto = &iof_protocol_registry[cls];
	struct rpc_data *rp = proto->rpc_types;

	for (i = 0 ; i < iof_protocol_registry->rpc_type_count ; i++) {
		int rc;

		rp->op_id = proto->id_base + i;
		if (handlers)
			rc = crt_rpc_srv_register(rp->op_id,
						  CRT_RPC_FEAT_NO_TIMEOUT,
						  &rp->fmt, handlers[i]);
		else
			rc = crt_rpc_register(rp->op_id,
					      CRT_RPC_FEAT_NO_TIMEOUT,
					      &rp->fmt);

		if (rc != 0)
			IOF_LOG_ERROR("Failed to register RPC %s, rc=%d\n",
				      rp->fmt.crf_name, rc);

		if (rc != 0 && ret == 0)
			ret = rc;
		rp++;
	}

	return ret;
}
