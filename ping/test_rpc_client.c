#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>

#include <mercury.h>
#include <mercury_types.h>

#include "my_rpc_common.h"

hg_return_t my_rpc_test_cb(const struct hg_cb_info *info);

int main(int argc, char **argv)
{
    hg_id_t         my_rpc_id;
    na_class_t      *na_class = NULL;
    na_context_t    *na_context = NULL;
    hg_class_t      *hg_class = NULL;
    hg_context_t    *hg_context = NULL;
    hg_handle_t     my_hg_handle;
    na_addr_t       my_server_addr;
    struct my_rpc_test_state *my_rpc_test_state_p;
    my_rpc_test_in_t in_struct;
    unsigned int act_count = 0;
    hg_return_t ret;

    my_rpc_test_state_p = malloc(sizeof(struct my_rpc_test_state));
//    char *uri       = "tcp://localhost:1234";
    char *uri       = "bmi+tcp://localhost:8888";

    na_class = NA_Initialize(uri, NA_FALSE);
    assert(na_class);
    na_context = NA_Context_create(na_class);
    assert(na_context);
    hg_class = HG_Init(na_class, na_context, NULL);
    assert(hg_class);
    hg_context = HG_Context_create(hg_class);
    assert(hg_context);

    my_rpc_id = HG_Register(hg_class, "rpc_test", my_in_proc_cb, my_out_proc_cb, my_rpc_test_handler);
    NA_Addr_lookup_wait(na_class, uri, &my_server_addr);
    HG_Create(hg_class, hg_context, my_server_addr, my_rpc_id, &my_hg_handle);
    my_rpc_test_state_p->cc = 18;
    in_struct.aa = 19;
    HG_Forward(my_hg_handle, my_rpc_test_cb, my_rpc_test_state_p, &in_struct);



    fprintf(stdout, "I am here\n");
    while (1) {
        do {
            ret = HG_Trigger(hg_class, hg_context, 0, 1, &act_count);
        } while (ret == HG_SUCCESS && act_count);
        HG_Progress(hg_class, hg_context, 100);
        if (act_count) break;
    }
//    sleep(10);
    HG_Context_destroy(hg_context);
    HG_Finalize(hg_class);
    NA_Context_destroy(na_class, na_context);
    NA_Finalize(na_class);
}


hg_return_t my_rpc_test_cb(const struct hg_cb_info *info)
{
    my_rpc_test_out_t out_struct;

    HG_Get_output(info->handle, &out_struct);
    fprintf(stdout, "rpc_test finished on remote node, ");
    fprintf(stdout, "return value: %d\n", out_struct.bb);

    return 0;
}
