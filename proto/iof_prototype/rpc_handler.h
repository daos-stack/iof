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
#include <sys/stat.h>
#include <mercury.h>
#include <mercury_macros.h>
#include <mcl_event.h>
#include "server_backend.h"

#ifndef RPC_HANDLER_H
#define RPC_HANDLER_H

struct rpc_id {
	hg_id_t getattr_id;
	hg_id_t readdir_id;
	hg_id_t mkdir_id;
	hg_id_t rmdir_id;
	hg_id_t symlink_id;
	hg_id_t readlink_id;
	hg_id_t unlink_id;
};
/* Common struct for RPC types which send a single string */
struct rpc_request_string {
	const char *name;
};

/* Common struct for RPC types which reply with a single string */
struct rpc_reply_basic {
	uint64_t error_code;
};

/* Types for RPCs which have custom parameters */

struct getattr_out_t {
	struct stat stbuf;
	uint64_t error_code;
};

/*readdir()*/
struct readdir_in_t {
	const char *dir_name;
	uint64_t offset;
};

struct readdir_out_t {
	char name[255];
	struct stat stat;
	uint64_t error_code;
	uint64_t complete;
};

/*mkdir()*/
struct mkdir_in_t {
	const char *name;
	mode_t mode;
};

/*symlink*/
struct symlink_in_t {
	const char *name;
	const char *dst;
};

struct readlink_out_t {
	char *dst;
	uint64_t error_code;
};

hg_return_t getattr_out_proc_cb(hg_proc_t proc, void *data);
hg_id_t getattr_register(hg_class_t *);

hg_id_t readdir_register(hg_class_t *);
hg_return_t readdir_in_proc_cb(hg_proc_t, void *data);
hg_return_t readdir_out_proc_cb(hg_proc_t proc, void *data);

hg_id_t mkdir_register(hg_class_t *);
hg_return_t mkdir_in_proc_cb(hg_proc_t, void *data);

hg_id_t rmdir_register(hg_class_t *);

hg_id_t symlink_register(hg_class_t *);
hg_return_t symlink_in_proc_cb(hg_proc_t proc, void *data);

hg_id_t readlink_register(hg_class_t *);
hg_return_t readlink_out_proc_cb(hg_proc_t proc, void *data);

hg_id_t unlink_register(hg_class_t *);
#endif
