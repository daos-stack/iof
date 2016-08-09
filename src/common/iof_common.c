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
