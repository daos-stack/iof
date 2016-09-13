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
		return -ENOMEM;

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

/*
 * Process filesystem query from CNSS
 * This function currently uses dummy data to send back to CNSS
 */
hg_return_t iof_query_handler(hg_handle_t handle)
{
	struct iof_psr_query query;
	struct iof_fs_info *list = NULL;
	int i;
	int ret;

	/* This is random made-up value for now */
	query.num = 2;

	list = calloc(query.num, sizeof(struct iof_fs_info));
	if (!list) {
		IOF_LOG_ERROR("Failed memory allocation");
		return -ENOMEM;
	}
	for (i = 0; i < query.num; i++) {
		list[i].mode = 0;
		list[i].id = i;
		sprintf(list[i].mnt, "/Rank%d", i);
	}
	query.list = list;
	ret = HG_Respond(handle, NULL, NULL, &query);
	if (ret != HG_SUCCESS)
		IOF_LOG_ERROR("RPC response not sent from IONSS");
	free(list);
	return ret;
}
