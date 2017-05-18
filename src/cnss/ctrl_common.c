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
#include <inttypes.h>
#include <string.h>
#include <crt_util/clog.h>
#include "ctrl_common.h"

static int iof_uint_read(char *buf, size_t buflen, void *arg)
{
	uint *value = (uint *)arg;

	snprintf(buf, buflen, "%u", *value);
	return 0;
}

static int shutdown_cb(void *arg)
{
	IOF_LOG_INFO("Stopping ctrl fs");
	ctrl_fs_stop();

	IOF_LOG_INFO("Invoking client shutdown");

	return cnss_shutdown(arg);
}

#define MAX_MASK_LEN 256
static int log_mask_cb(const char *mask,  void *cb_arg)
{
	char newmask[MAX_MASK_LEN];
	char *pos;
	size_t len;

	if (strcmp(mask, "\n") == 0 || strlen(mask) == 0) {
		IOF_LOG_INFO("No log mask specified, resetting to ERR");
		strcpy(newmask, "ERR");
	} else {
		/* strip '\n' */
		pos = strchr(mask, '\n');
		if (pos != NULL)
			len = ((uintptr_t)pos - (uintptr_t)mask);
		else
			len = strlen(mask);

		if (len > MAX_MASK_LEN - 1)
			len = MAX_MASK_LEN - 1;

		strncpy(newmask, mask, len);
		newmask[len] = 0;

		IOF_LOG_INFO("Setting log mask to %s", newmask);
	}

	crt_log_setmasks(newmask, strlen(newmask));

	return 0;
}


int register_cnss_controls(struct cnss_info *cnss_info)
{
	char *crt_protocol;
	int ret;
	int rc = 0;

	ctrl_register_variable(NULL, "active",
			       iof_uint_read,
			       NULL, NULL,
			       &cnss_info->active);

	ret = ctrl_register_event(NULL, "shutdown",
				  shutdown_cb /* trigger_cb */,
				  NULL /* destroy_cb */, (void *)cnss_info);
	if (ret != 0) {
		IOF_LOG_ERROR("Could not register shutdown ctrl");
		rc = ret;
		ctrl_fs_stop();
	}

	ret = ctrl_register_variable(NULL, "log_mask",
				  NULL /* read_cb */,
				  log_mask_cb /* write_cb */,
				  NULL /* destroy_cb */, NULL);
	if (ret != 0) {
		IOF_LOG_ERROR("Could not register log_mask ctrl");
		rc = ret;
		ctrl_fs_stop();
	}

	ret = ctrl_register_constant_int64(NULL, "cnss_id", getpid());
	if (ret != 0) {
		IOF_LOG_ERROR("Could not register cnss_id");
		rc = ret;
		ctrl_fs_stop();
	}

	crt_protocol = getenv("CRT_PHY_ADDR_STR");
	if (crt_protocol) /* Only register if set */
		ctrl_register_constant(NULL, "crt_protocol", crt_protocol);

	return rc;
}
