#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sched.h>

#include <mercury.h>
#include <mercury_types.h>
#include <mercury_proc_string.h>

hg_return_t my_in_proc_cb(hg_proc_t proc, void *data);
hg_return_t my_out_proc_cb(hg_proc_t proc, void *data);
hg_return_t my_rpc_test_handler(hg_handle_t handle);

struct my_rpc_test_state {
    int32_t cc;
};
typedef struct {
    int32_t aa;
} my_rpc_test_in_t;


typedef struct {
    int32_t bb;
} my_rpc_test_out_t;
