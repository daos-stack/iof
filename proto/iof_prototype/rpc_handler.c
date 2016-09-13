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
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include "rpc_handler.h"
#include "server_backend.h"
#include <mercury_proc_string.h>
#include <errno.h>
#include "iof_test_log.h"

hg_return_t string_in_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct rpc_request_string *struct_data = data;

	ret = hg_proc_hg_const_string_t(proc, &struct_data->name);
	assert(ret == HG_SUCCESS);
	return ret;
}

hg_return_t basic_out_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct rpc_reply_basic *struct_data = data;

	ret = hg_proc_hg_uint64_t(proc, &struct_data->error_code);
	assert(ret == HG_SUCCESS);

	return ret;
}

static hg_return_t getattr_handler(hg_handle_t handle)
{
	struct rpc_request_string in;
	struct getattr_out_t out = {0};
	uint64_t ret;
	struct stat *buf;

	ret = HG_Get_input(handle, &in);
	assert(ret == HG_SUCCESS);
	IOF_TESTLOG_INFO("got RPC");

	buf = &out.stbuf;
	/* send arguments to the local filesystem */
	IOF_TESTLOG_INFO("Calling getattr %s %p", in.name, (void *)buf);
	out.error_code = iof_getattr(in.name, buf);
	ret = HG_Free_input(handle, &in);
	assert(ret == HG_SUCCESS);

	ret = HG_Respond(handle, NULL, NULL, &out);
	assert(ret == HG_SUCCESS);
	return ret;
}

static hg_return_t readdir_handler(hg_handle_t handle)
{
	struct readdir_in_t in;
	struct readdir_out_t out = {0};
	uint64_t ret;

	ret = HG_Get_input(handle, &in);
	assert(ret == HG_SUCCESS);
	/* in has the input args now */
	IOF_TESTLOG_INFO("got  RPC");

	out.complete =
	    iof_readdir(out.name, in.dir_name, &out.error_code, in.offset,
			&out.stat);
	ret = HG_Free_input(handle, &in);
	assert(ret == HG_SUCCESS);
	ret = HG_Respond(handle, NULL, NULL, &out);
	assert(ret == HG_SUCCESS);
	return ret;
}

static hg_return_t mkdir_handler(hg_handle_t handle)
{
	struct mkdir_in_t in;
	struct rpc_reply_basic out = {0};
	hg_return_t ret;

	ret = HG_Get_input(handle, &in);
	assert(ret == HG_SUCCESS);

	out.error_code = iof_mkdir(in.name, in.mode);
	ret = HG_Free_input(handle, &in);
	assert(ret == HG_SUCCESS);
	ret = HG_Respond(handle, NULL, NULL, &out);
	assert(ret == HG_SUCCESS);

	return ret;
}

static hg_return_t rmdir_handler(hg_handle_t handle)
{
	struct rpc_request_string in;
	struct rpc_reply_basic out = {0};
	int ret;

	ret = HG_Get_input(handle, &in);
	assert(ret == HG_SUCCESS);

	out.error_code = iof_rmdir(in.name);
	ret = HG_Free_input(handle, &in);
	assert(ret == HG_SUCCESS);
	ret = HG_Respond(handle, NULL, NULL, &out);
	assert(ret == HG_SUCCESS);

	return ret;
}

static hg_return_t symlink_handler(hg_handle_t handle)
{
	struct symlink_in_t in;
	struct rpc_reply_basic out = {0};
	int ret;

	ret = HG_Get_input(handle, &in);
	assert(ret == HG_SUCCESS);

	out.error_code = iof_symlink(in.dst, in.name);
	ret = HG_Free_input(handle, &in);
	assert(ret == HG_SUCCESS);

	ret = HG_Respond(handle, NULL, NULL, &out);
	assert(ret == HG_SUCCESS);

	return ret;
}

static hg_return_t readlink_handler(hg_handle_t handle)
{
	struct rpc_request_string in;
	struct readlink_out_t out = {0};
	int ret;

	ret = HG_Get_input(handle, &in);
	assert(ret == HG_SUCCESS);

	out.error_code = iof_readlink(in.name, &out.dst);

	ret = HG_Free_input(handle, &in);
	assert(ret == HG_SUCCESS);

	ret = HG_Respond(handle, NULL, NULL, &out);
	assert(ret == HG_SUCCESS);

	return ret;
}

static hg_return_t unlink_handler(hg_handle_t handle)
{
	struct rpc_request_string in;
	struct rpc_reply_basic out = {0};
	int ret;

	ret = HG_Get_input(handle, &in);
	assert(ret == HG_SUCCESS);

	out.error_code = iof_unlink(in.name);

	ret = HG_Free_input(handle, &in);
	assert(ret == HG_SUCCESS);

	ret = HG_Respond(handle, NULL, NULL, &out);
	assert(ret == HG_SUCCESS);

	return ret;
}

/* registration functions for client/server */
hg_id_t getattr_register(hg_class_t *rpc_class)
{
	hg_id_t tmp;

	tmp = HG_Register_name(rpc_class, "getattr", string_in_proc_cb,
			       getattr_out_proc_cb, getattr_handler);
	return tmp;
}

hg_id_t readdir_register(hg_class_t *rpc_class)
{
	hg_id_t tmp;

	tmp = HG_Register_name(rpc_class, "readdir", readdir_in_proc_cb,
			       readdir_out_proc_cb, readdir_handler);
	return tmp;
}

hg_id_t mkdir_register(hg_class_t *rpc_class)
{
	hg_id_t tmp;

	tmp =
	    HG_Register_name(rpc_class, "mkdir", mkdir_in_proc_cb,
			     basic_out_proc_cb, mkdir_handler);
	return tmp;
}

hg_id_t rmdir_register(hg_class_t *rpc_class)
{
	hg_id_t tmp;

	tmp =
	    HG_Register_name(rpc_class, "rmdir", string_in_proc_cb,
			     basic_out_proc_cb, rmdir_handler);
	return tmp;
}

hg_id_t symlink_register(hg_class_t *rpc_class)
{
	hg_id_t tmp;

	tmp =
	    HG_Register_name(rpc_class, "symlink", symlink_in_proc_cb,
			     basic_out_proc_cb, symlink_handler);
	return tmp;
}

hg_id_t readlink_register(hg_class_t *rpc_class)
{
	hg_id_t tmp;

	tmp =
	    HG_Register_name(rpc_class, "readlink", string_in_proc_cb,
			     readlink_out_proc_cb, readlink_handler);
	return tmp;
}

hg_id_t unlink_register(hg_class_t *rpc_class)
{
	hg_id_t tmp;

	tmp =
	    HG_Register_name(rpc_class, "unlink", string_in_proc_cb,
			     basic_out_proc_cb, unlink_handler);
	return tmp;
}

hg_return_t getattr_out_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct getattr_out_t *struct_data = data;

	ret =
	    hg_proc_raw(proc, &struct_data->stbuf, sizeof(struct_data->stbuf));

	assert(ret == HG_SUCCESS);

	ret = hg_proc_hg_uint64_t(proc, &struct_data->error_code);
	assert(ret == HG_SUCCESS);
	return ret;
}

hg_return_t readdir_in_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct readdir_in_t *struct_data = data;
	/* Dir_name */
	ret = hg_proc_hg_const_string_t(proc, &struct_data->dir_name);
	assert(ret == HG_SUCCESS);
	/*offset */
	ret = hg_proc_hg_uint64_t(proc, &struct_data->offset);
	assert(ret == HG_SUCCESS);
	return ret;
}

hg_return_t readdir_out_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct readdir_out_t *struct_data = data;
	/* name */
	ret = hg_proc_raw(proc, &struct_data->name, sizeof(struct_data->name));
	assert(ret == HG_SUCCESS);
	ret = hg_proc_hg_uint64_t(proc, &struct_data->error_code);
	assert(ret == HG_SUCCESS);
	ret = hg_proc_hg_uint64_t(proc, &struct_data->complete);
	assert(ret == HG_SUCCESS);
	ret = hg_proc_raw(proc, &struct_data->stat, sizeof(struct stat));
	assert(ret == HG_SUCCESS);
	return ret;
}

hg_return_t mkdir_in_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct mkdir_in_t *struct_data = data;

	ret = hg_proc_hg_const_string_t(proc, &struct_data->name);
	assert(ret == HG_SUCCESS);

	ret = hg_proc_raw(proc, &struct_data->mode, sizeof(struct_data->mode));
	assert(ret == HG_SUCCESS);

	return ret;
}

hg_return_t symlink_in_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct symlink_in_t *struct_data = data;

	ret = hg_proc_hg_const_string_t(proc, &struct_data->name);
	assert(ret == HG_SUCCESS);

	ret = hg_proc_hg_const_string_t(proc, &struct_data->dst);
	assert(ret == HG_SUCCESS);
	return ret;
}

hg_return_t readlink_out_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct readlink_out_t *struct_data = data;

	ret = hg_proc_hg_string_t(proc, &struct_data->dst);
	assert(ret == HG_SUCCESS);

	ret = hg_proc_hg_uint64_t(proc, &struct_data->error_code);
	assert(ret == HG_SUCCESS);

	return ret;
}

