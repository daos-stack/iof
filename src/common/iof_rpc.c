/* Copyright (C) 2016-2018 Intel Corporation
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
#include "iof_fs.h"
#include "log.h"

#define IOF_PROTO_WRITE_BASE 0x01000000
#define IOF_PROTO_SIGNON_BASE 0x02000000
#define IOF_PROTO_SIGNON_VERSION 2

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

struct crt_msg_field *imigrate_in[] = {
	&CMF_GAH,	/* gah of parent */
	&CMF_IOF_NAME,	/* name */
	&CMF_INT,	/* inode */
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
};

struct crt_msg_field *readdir_out[] = {
	&CMF_IOVEC,
	&CMF_INT,
	&CMF_INT,
	&CMF_INT,
	&CMF_INT,
};

struct crt_msg_field *psr_out[] = {
	&CMF_IOVEC,
	&CMF_UINT32,
	&CMF_UINT32,
	&CMF_BOOL,
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

struct crt_msg_field *setattr_in[] = {
	&CMF_GAH,	/* gah */
	&CMF_IOF_STAT,	/* struct stat */
	&CMF_UINT32,	/* to_set */
};

/*query RPC format*/
struct crt_req_format QUERY_RPC_FMT = DEFINE_CRT_REQ_FMT(NULL, psr_out);

#define X(a, b, c)					\
	static struct crt_req_format IOF_CRF_##a =	\
		DEFINE_CRT_REQ_FMT(b, c);

IOF_RPCS_LIST

#undef X

#define X(a, b, c)					\
	{						\
		.prf_flags = CRT_RPC_FEAT_NO_TIMEOUT,	\
		.prf_req_fmt = &IOF_CRF_##a,	\
	},

static struct crt_proto_rpc_format iof_write_rpc_types[] = {
	IOF_RPCS_LIST
};

#undef X

static struct crt_proto_rpc_format iof_signon_rpc_types[] = {
	{
		.prf_req_fmt = &QUERY_RPC_FMT,
	},
	{
		.prf_flags = CRT_RPC_FEAT_NO_TIMEOUT,
	}
};

static struct crt_proto_format iof_signon_registry = {
	.cpf_name = "IOF_HANDSHAKE",
	.cpf_ver = IOF_PROTO_SIGNON_VERSION,
	.cpf_count = ARRAY_SIZE(iof_signon_rpc_types),
	.cpf_prf = iof_signon_rpc_types,
	.cpf_base = IOF_PROTO_SIGNON_BASE,
};

static struct crt_proto_format iof_write_registry = {
	.cpf_name = "IOF_WRITE",
	.cpf_ver = 2,
	.cpf_count = ARRAY_SIZE(iof_write_rpc_types),
	.cpf_prf = iof_write_rpc_types,
	.cpf_base = IOF_PROTO_WRITE_BASE,
};

/* Bulk register a RPC type
 *
 * If there is a failure then register what is possible, and return
 * the first error that occurred.
 *
 * On the origin side the handlers array can be NULL, as no RPCs are expected
 * to be received.
 * On the target side proto can be NULL, as no RPCs are sent, so the opcodes
 * are not requried.
 */
static int
iof_core_register(struct crt_proto_format *reg,
		  struct crt_proto_format **proto,
		  crt_rpc_cb_t handlers[])
{
	int rc;
	int i;

	if (handlers)
		for (i = 0; i < reg->cpf_count; i++)
			reg->cpf_prf[i].prf_hdlr = handlers[i];

	rc = crt_proto_register(reg);

	if (proto && rc == -DER_SUCCESS)
		*proto = reg;

	return rc;
}

int
iof_register(struct crt_proto_format **proto,
	     crt_rpc_cb_t handlers[])
{
	return iof_core_register(&iof_write_registry, proto, handlers);
}

int
iof_signon_register(crt_rpc_cb_t handlers[])
{
	return iof_core_register(&iof_signon_registry, NULL, handlers);
}

struct sq_cb {
	struct iof_tracker	tracker;
	uint32_t		version;
	int			rc;
};

static void
iof_signon_query_cb(struct crt_proto_query_cb_info *cb_info)
{
	struct sq_cb *cbi = cb_info->pq_arg;

	cbi->rc = cb_info->pq_rc;
	if (cbi->rc == -DER_SUCCESS) {
		cbi->version = cb_info->pq_ver;
	}

	iof_tracker_signal(&cbi->tracker);
}

/* Query the server side protocol in use, for now we only support one
 * version so check that with the server and ensure it's what we expect
 */
int
iof_signon_query(crt_endpoint_t *tgt_ep,
		 struct crt_proto_format **proto)
{
	uint32_t ver = IOF_PROTO_SIGNON_VERSION;
	struct sq_cb cbi = {0};
	int rc;

	iof_tracker_init(&cbi.tracker, 1);

	rc = crt_proto_query(tgt_ep, IOF_PROTO_SIGNON_BASE,
			     &ver, 1, iof_signon_query_cb, &cbi);
	if (rc != -DER_SUCCESS) {
		return rc;
	}

	iof_tracker_wait(&cbi.tracker);

	if (cbi.rc != -DER_SUCCESS) {
		return cbi.rc;
	}

	if (cbi.version != IOF_PROTO_SIGNON_VERSION) {
		return -DER_INVAL;
	}

	return iof_core_register(&iof_signon_registry, proto, NULL);
}
