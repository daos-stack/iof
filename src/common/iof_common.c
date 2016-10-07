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
#include <mercury.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include "iof_common.h"
#include "log.h"

/* Packing and unpacking function*/
hg_return_t iof_query_out_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct iof_psr_query *struct_data = (struct iof_psr_query *)data;
	int i;

	ret = hg_proc_hg_uint64_t(proc, &struct_data->num);
	if (ret != HG_SUCCESS) {
		IOF_LOG_ERROR("Cant pack args");
		return ret;
	}
	if (struct_data->list == NULL)
		struct_data->list = calloc(struct_data->num,
				sizeof(struct iof_fs_info));
	if (struct_data->list == NULL)
		return IOF_ERR_NOMEM;

	for (i = 0; i < struct_data->num; i++) {
		ret = hg_proc_hg_uint8_t(proc, &struct_data->list[i].mode);
		if (ret != HG_SUCCESS)
			return ret;
		ret = hg_proc_hg_uint64_t(proc, &struct_data->list[i].id);
		if (ret != HG_SUCCESS)
			return ret;
		ret = hg_proc_raw(proc, &struct_data->list[i].mnt,
				sizeof(struct_data->list[i].mnt));
		if (ret != HG_SUCCESS)
			return ret;
	}
	return ret;
}

hg_return_t iof_string_in_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct iof_string_in *in_data = (struct iof_string_in *)data;

	ret = hg_proc_hg_const_string_t(proc, &in_data->name);
	if (ret)
		IOF_LOG_ERROR("Could not pack string args");
	ret = hg_proc_hg_uint64_t(proc, &in_data->my_fs_id);
	if (ret)
		IOF_LOG_ERROR("Could not pack string args");
	return ret;
}

hg_return_t iof_getattr_out_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct iof_getattr_out *out_data = (struct iof_getattr_out *)data;

	ret = hg_proc_raw(proc, &out_data->stbuf, sizeof(out_data->stbuf));

	if (ret)
		IOF_LOG_ERROR("Could not pack getattr output");

	ret = hg_proc_hg_uint64_t(proc, &out_data->err);
	if (ret)
		IOF_LOG_ERROR("Could not pack getattr output");
	return ret;
}

/*
 * Process filesystem query from CNSS
 * This function currently uses dummy data to send back to CNSS
 */
hg_return_t iof_query_handler(hg_handle_t handle)
{
	struct iof_psr_query *query;
	int ret;
	struct hg_info *hgi;

	hgi = HG_Get_info(handle);
	if (!hgi)
		return IOF_ERR_INTERNAL;
	query = (struct iof_psr_query *)HG_Registered_data(hgi->hg_class,
								hgi->id);
	ret = HG_Respond(handle, NULL, NULL, query);
	if (ret != HG_SUCCESS)
		IOF_LOG_ERROR("RPC response not sent from IONSS");
	return ret;
}

int iof_get_path(int id, const char *old_path,
		struct iof_psr_query *projection, char *new_path)
{
	char *mnt;
	int ret;

	/*lookup mnt by ID in projection data structure*/
	if (id >= projection->num) {
		IOF_LOG_ERROR("Filesystem ID invalid");
		return IOF_BAD_DATA;
	}

	mnt = projection->list[id].mnt;
	/*add io node tempdir prefix*/
	ret = snprintf(new_path, IOF_MAX_PATH_LEN, "%s/%s%s", ion_tempdir, mnt,
			old_path);
	if (ret > IOF_MAX_PATH_LEN)
		return IOF_ERR_OVERFLOW;
	IOF_LOG_DEBUG("New Path: %s", new_path);

	return IOF_SUCCESS;
}

hg_return_t iof_getattr_handler(hg_handle_t handle)
{
	struct iof_string_in in;
	struct iof_getattr_out out = {0};
	uint64_t ret;
	struct hg_info *hgi;
	char new_path[IOF_MAX_PATH_LEN];
	struct iof_psr_query *query;

	hgi = HG_Get_info(handle);
	if (!hgi)
		return IOF_ERR_INTERNAL;
	query = (struct iof_psr_query *)HG_Registered_data(hgi->hg_class,
								hgi->id);
	ret = HG_Get_input(handle, &in);
	if (ret)
		IOF_LOG_ERROR("Could not retreive input");
	IOF_LOG_DEBUG("RPC_recieved with path:%s", in.name);
	memset(&new_path[0], 0, sizeof(new_path));
	ret = (uint64_t)iof_get_path(in.my_fs_id, in.name, query, &new_path[0]);
	if (ret) {
		IOF_LOG_ERROR("could not construct filesystem path, ret = %lu",
				ret);
		out.err = ret;
	} else
		out.err = stat(new_path, &out.stbuf);
	ret = HG_Respond(handle, NULL, NULL, &out);
	if (ret)
		IOF_LOG_ERROR("getattr: response not sent from IONSS");
	ret = HG_Free_input(handle, &in);
	if (ret)
		IOF_LOG_ERROR("Could not free input");
	return ret;
}
