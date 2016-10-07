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
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include "version.h"
#include "log.h"
#include <mercury.h>
#include <process_set.h>
#include "iof_common.h"


void ionss_register(struct mcl_state *state, struct iof_psr_query *projection)
{
	hg_id_t query_id, getattr_id;

	query_id = HG_Register_name(state->hg_class, "Projection_query",
			NULL, iof_query_out_proc_cb, iof_query_handler);
	HG_Register_data(state->hg_class, query_id, projection, NULL);
	IOF_LOG_INFO("Id registered on server:%d", query_id);
	getattr_id = HG_Register_name(state->hg_class, "getattr",
			iof_string_in_proc_cb,
			iof_getattr_out_proc_cb, iof_getattr_handler);
	HG_Register_data(state->hg_class, getattr_id, projection, NULL);
}

int main(int argc, char **argv)
{
	struct mcl_set *set;
	struct mcl_state *proc_state;
	int i;
	struct iof_fs_info *fs_list = NULL;
	int ret = IOF_SUCCESS;
	struct iof_psr_query *projection;

	char *version = iof_get_version();
	ion_tempdir = getenv("ION_TEMPDIR");

	iof_log_init("ionss");
	IOF_LOG_INFO("IONSS version: %s", version);
	projection = calloc(1, sizeof(struct iof_psr_query));
	if (!projection)
		return IOF_ERR_NOMEM;
	if (argc < 2) {
		IOF_LOG_ERROR("Expected at least one backend IONSS temp dir as command line option");
		return IOF_BAD_DATA;
	}
	projection->num = argc - 1;
	/*hardcoding the number and path for projected filesystems*/
	fs_list = calloc(projection->num, sizeof(struct iof_fs_info));
	if (!fs_list) {
		IOF_LOG_ERROR("Filesystem list not allocated");
		ret = IOF_ERR_NOMEM;
		goto cleanup;
	}

	for (i = 0; i < projection->num; i++) {

		fs_list[i].mode = 0;
		fs_list[i].id = i;
		sprintf(fs_list[i].mnt, argv[i+1]);
		IOF_LOG_DEBUG("Created directory: %s", fs_list[i].mnt);
	}

	projection->list = fs_list;
	proc_state = mcl_init();
	if (proc_state == NULL) {
		IOF_LOG_ERROR("mcl_init() failed.");
		ret = IOF_ERR_MCL;
		goto cleanup;
	}
	ionss_register(proc_state, projection);
	mcl_startup(proc_state, "ionss", 1, &set);
	IOF_LOG_INFO("name %s size %d rank %d is_local %d is_service %d",
		     set->name, set->size, set->self, set->is_local,
		     set->is_service);


cleanup:
	if (proc_state)
		mcl_finalize(proc_state);
	if (projection->list)
		free(projection->list);
	if (projection)
		free(projection);

	iof_log_close();

	return ret;
}
