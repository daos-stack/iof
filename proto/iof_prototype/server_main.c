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
#include <assert.h>
#include<unistd.h>
#include<stdio.h>
#include "rpc_handler.h"
#include <process_set.h>
#include "iof_test_log.h"

/* Will be the wrapper on top of the file system that will register
 * and handle the RPC. It will execute indefinitely.
 */

/* This function will basically create ID's and register all function calls on
 * the client. All register functions are defined in client_handler.c
 */
struct rpc_id rpc_id;

void server_init(hg_class_t *rpc_class)
{
	rpc_id.readdir_id = readdir_register(rpc_class);

	rpc_id.getattr_id = getattr_register(rpc_class);

	rpc_id.mkdir_id = mkdir_register(rpc_class);

	rpc_id.rmdir_id = rmdir_register(rpc_class);

	rpc_id.symlink_id = symlink_register(rpc_class);

	rpc_id.readlink_id = readlink_register(rpc_class);

	rpc_id.unlink_id = unlink_register(rpc_class);
}

int main(int argc, char **argv)
{
	hg_class_t *rpc_class = NULL;
	struct mcl_set *set;
	struct mcl_state *proc_state;
	int is_service = 1;
	char *name_of_set = "server";


	iof_testlog_init("server_main");
	filesystem_init();
	iof_mkdir("/started", 0600);
	proc_state = mcl_init();
	rpc_class = proc_state->hg_class;
	server_init(rpc_class);
	mcl_startup(proc_state, name_of_set, is_service, &set);
	mcl_finalize(proc_state);
	iof_testlog_close();
	return 0;

}
