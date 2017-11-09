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
#include <cart/api.h>
#include <gurt/common.h>

#include "ios_gah.h"
#include "iof_ext.h"

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
#define IOF_ERR_CTRL_FS        11

#define IOF_NAME_LEN_MAX 256

/* IOF Registration op codes for RPC's*/

#define QUERY_PSR_OP	(0x201)
#define DETACH_OP	(0x202)

#define IOF_DEFAULT_SET "IONSS"

/*
 * IOF features are represented by an 8-bit unsigned bit vector
 * \ref iof_fs_info.flags and are used turn various features on or off.
 * A combination of different features defines the projection mode
 *
 * Features that don't require separate implementations:
 * Bit [0]	: 0=Read-Only, 1=Read-Write
 * Bit [1]	: Failover [0=Off, 1=On]
 *
 * Features that may require separate implementations::
 * Bit [2]	: Striped Metadata [0=Off, 1=On]
 * Bit [3]	: Striped Data [0=Off, 1=On]
 *
 * Features of the projected storage type:
 * Bit [5,4]	: 00=Default, 01=Lustre, 010=DW-Scratch, 011=DW-Cache
 *
 */
#define IOF_FS_DEFAULT			0x00
#define IOF_FS_LUSTRE			0x10
#define IOF_DW_SCRATCH			0x20
#define IOF_DW_CACHE			0x30

#define IOF_WRITEABLE			0x01
#define IOF_FAILOVER			0x02
#define IOF_STRIPED_METADATA		0x04
#define IOF_STRIPED_DATA		0x08

#define IOF_IS_WRITEABLE(FLAGS) ((FLAGS) & IOF_WRITEABLE)
#define IOF_HAS_FAILOVER(FLAGS) ((FLAGS) & IOF_FAILOVER)

#define IOF_CNSS_MT			0x80

enum iof_projection_mode {
	/* Private Access Mode */
	IOF_DEFAULT_PRIVATE,
	/* Striped Metadata on PFS */
	IOF_PFS_STRIPED_METADATA,
	/* Striped Data on PFS */
	IOF_PFS_STRIPED_DATA,
	/* Striped Metadata on Lustre */
	IOF_LUSTRE_STRIPED_METADATA,

	/* Data Warp [Scratch], Private */
	IOF_DWS_PRIVATE,
	/* Data Warp [Cache], Private */
	IOF_DWC_PRIVATE,
	/* Data Warp [Scratch], Striped Data */
	IOF_DWS_STRIPED_DATA,
	/* Data Warp [Cache], Striped Data */
	IOF_DWC_STRIPED_DATA,

	/* Total number of Projection Modes */
	IOF_PROJECTION_MODES
};

struct iof_fs_info {
	/*Associated mount point*/
	char mnt[IOF_NAME_LEN_MAX];
	/*id of filesystem*/
	struct ios_gah gah;
	int id;
	/*Feature flags, as described above*/
	uint8_t flags;
};

/* The response to the initial query RPC.
 * Note that readdir_size comes after the IOV in order to avoid
 * the compiler automatically padding the struct.
 */
struct iof_psr_query {
	uint32_t max_read;
	uint32_t max_write;
	d_iov_t query_list;
	uint32_t readdir_size;
	uint32_t max_iov_read;
	uint32_t max_iov_write;
};

struct iof_gah_string_in {
	struct ios_gah gah;
	d_string_t path;
};

struct iof_string_in {
	d_string_t path;
	uint32_t fs_id;
};

struct iof_string_out {
	d_string_t path;
	int rc;
	int err;
};

struct iof_lookup_out {
	struct ios_gah gah;
	d_iov_t stat;
	int rc;
	int err;
};

struct iof_create_out {
	struct ios_gah gah;
	struct ios_gah igah;
	d_iov_t stat;
	int rc;
	int err;
};

struct iof_truncate_in {
	d_string_t path;
	uint64_t len;
	uint32_t fs_id;
};

struct iof_ftruncate_in {
	struct ios_gah gah;
	uint64_t len;
};

struct iof_two_string_in {
	struct ios_gah gah;
	d_string_t src;
	d_string_t dst;
};

struct iof_create_in {
	d_string_t path;
	struct ios_gah gah;
	uint32_t mode;
	uint32_t flags;
	uint32_t reg_inode;
};

struct iof_rename_in {
	struct ios_gah old_gah;
	struct ios_gah new_gah;
	d_string_t old_path;
	d_string_t new_path;
	uint32_t flags;
};

struct iof_open_in {
	struct ios_gah gah;
	d_string_t path;
	uint32_t flags;
};

struct iof_getattr_out {
	d_iov_t stat;
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
	crt_bulk_t bulk;
	uint64_t offset;
	uint32_t fs_id;
};

/* Each READDIR rpc contains an array of these */
struct iof_readdir_reply {
	char d_name[256];
	struct stat stat;
	off_t nextoff;
	int read_rc;
	int stat_rc;
};

struct iof_readdir_out {
	d_iov_t replies;
	int last;
	int iov_count;
	int bulk_count;
	int err;
};

struct iof_open_out {
	struct ios_gah gah;
	int rc;
	int err;
};


struct iof_readx_in {
	struct ios_gah gah;
	struct iof_xtvec xtvec;
	uint64_t xtvec_len;
	uint64_t bulk_len;
	crt_bulk_t xtvec_bulk;
	crt_bulk_t data_bulk;
};

struct iof_readx_out {
	d_iov_t data;
	uint64_t bulk_len;
	uint32_t iov_len;
	int rc;
	int err;
};

struct iof_data_out {
	d_iov_t data;
	int rc;
	int err;
};

struct iof_status_out {
	int rc;
	int err;
};

struct iof_gah_in {
	struct ios_gah gah;
};

struct iof_writex_in {
	struct ios_gah gah;
	d_iov_t data;
	struct iof_xtvec xtvec;
	uint64_t xtvec_len;
	uint64_t bulk_len;
	crt_bulk_t xtvec_bulk;
	crt_bulk_t data_bulk;
};

struct iof_writex_out {
	uint64_t len;
	int rc;
	int err;
	uint64_t pad[2]; /* TODO: Optimize this later.  For now, just add
			  * some padding so ionss_io_req_desc fits
			  */
};

struct iof_chmod_in {
	d_string_t path;
	uint32_t fs_id;
	int mode;
};

struct iof_chmod_gah_in {
	struct ios_gah gah;
	int mode;
};

struct iof_time_in {
	d_string_t path;
	d_iov_t time;
	uint32_t fs_id;

};

struct iof_time_gah_in {
	struct ios_gah gah;
	d_iov_t time;
};

struct iof_setattr_in {
	struct ios_gah gah;
	d_iov_t attr;
	uint32_t to_set;
};

extern struct crt_req_format QUERY_RPC_FMT;

struct rpc_data {
	struct crt_req_format fmt;
	crt_opcode_t op_id;
};

#define DEF_PROTO_CLASS(NAME) IOF_PROTO_##NAME
#define DEF_RPC_TYPE(CLASS, TYPE) IOF_##CLASS##_##TYPE

enum iof_rpc_type_default {
	DEF_RPC_TYPE(DEFAULT, opendir),
	DEF_RPC_TYPE(DEFAULT, readdir),
	DEF_RPC_TYPE(DEFAULT, closedir),
	DEF_RPC_TYPE(DEFAULT, getattr),
	DEF_RPC_TYPE(DEFAULT, getattr_gah),
	DEF_RPC_TYPE(DEFAULT, writex),
	DEF_RPC_TYPE(DEFAULT, truncate),
	DEF_RPC_TYPE(DEFAULT, ftruncate),
	DEF_RPC_TYPE(DEFAULT, rmdir),
	DEF_RPC_TYPE(DEFAULT, rename),
	DEF_RPC_TYPE(DEFAULT, rename_ll),
	DEF_RPC_TYPE(DEFAULT, readx),
	DEF_RPC_TYPE(DEFAULT, unlink),
	DEF_RPC_TYPE(DEFAULT, open),
	DEF_RPC_TYPE(DEFAULT, create),
	DEF_RPC_TYPE(DEFAULT, close),
	DEF_RPC_TYPE(DEFAULT, mkdir),
	DEF_RPC_TYPE(DEFAULT, readlink),
	DEF_RPC_TYPE(DEFAULT, readlink_ll),
	DEF_RPC_TYPE(DEFAULT, symlink),
	DEF_RPC_TYPE(DEFAULT, fsync),
	DEF_RPC_TYPE(DEFAULT, fdatasync),
	DEF_RPC_TYPE(DEFAULT, chmod),
	DEF_RPC_TYPE(DEFAULT, chmod_gah),
	DEF_RPC_TYPE(DEFAULT, utimens),
	DEF_RPC_TYPE(DEFAULT, utimens_gah),
	DEF_RPC_TYPE(DEFAULT, statfs),
	DEF_RPC_TYPE(DEFAULT, lookup),
	DEF_RPC_TYPE(DEFAULT, setattr),
	IOF_DEFAULT_RPC_TYPES,
};

enum iof_proto_class {
	DEF_PROTO_CLASS(DEFAULT),
	IOF_PROTO_CLASSES
};

struct proto {
	char name[16];
	crt_opcode_t id_base;
	int rpc_type_count;
	struct rpc_data *rpc_types;
};

extern const struct proto iof_protocol_registry[];

int iof_register(enum iof_proto_class cls, crt_rpc_cb_t handlers[]);

#endif
