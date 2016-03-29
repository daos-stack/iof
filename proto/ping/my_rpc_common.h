#ifndef MY_RPC_COMMON_H
#define MY_RPC_COMMON_H

#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sched.h>

#include <mercury.h>
#include <mercury_bulk.h>
#include <mercury_proc_string.h>

hg_return_t my_in_proc_cb(hg_proc_t proc, void *data);
hg_return_t my_out_proc_cb(hg_proc_t proc, void *data);
hg_return_t my_rpc_test_handler(hg_handle_t handle);

extern hg_bool_t test_bulk_cb_done_g;

struct my_rpc_test_state {
	int32_t cc;
	hg_size_t size;
	void *buffer;
};

struct my_rpc_test_in_t {
	int32_t aa;
	hg_bulk_t bulk_handle;
};

struct my_rpc_test_out_t {
	int32_t bb;
};

#endif
