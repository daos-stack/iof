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
#include "ctrl_common.h"

static int shutdown_cb(void *arg)
{
	IOF_LOG_INFO("Stopping ctrl fs");
	ctrl_fs_stop();

	IOF_LOG_INFO("Invoking client shutdown");

	return cnss_shutdown(arg);
}

static int client_attach_cb(int client_id, void *arg)
{
	IOF_LOG_INFO("Client attached %d", client_id);

	return cnss_client_attach(client_id, arg);
}

static int client_detach_cb(int client_id, void *arg)
{
	IOF_LOG_INFO("Client detached %d", client_id);

	return cnss_client_detach(client_id, arg);
}

int register_cnss_controls(int count_start, void *arg)
{
	int ret;
	int rc = 0;

	ret = ctrl_register_event(NULL, "shutdown",
				  shutdown_cb /* trigger_cb */,
				  NULL /* destroy_cb */, arg);
	if (ret != 0) {
		IOF_LOG_ERROR("Could not register shutdown ctrl");
		rc = ret;
		ctrl_fs_stop();
	}

	ret = ctrl_register_counter(NULL, "client",
				    count_start /* start */,
				    1, /* increment */
				    client_attach_cb /* open_cb */,
				    client_detach_cb /* close_cb */,
				    NULL /* destroy_cb */,
				    arg);
	if (ret != 0) {
		IOF_LOG_ERROR("Could not register client ctrl");
		rc = ret;
		ctrl_fs_stop();
	}

	return rc;
}
