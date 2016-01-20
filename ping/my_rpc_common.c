#include "my_rpc_common.h"

hg_return_t my_in_proc_cb(hg_proc_t proc, void *data)
{
    hg_return_t ret;

    my_rpc_test_in_t *in_data;

    in_data = (my_rpc_test_in_t *) data;
    ret = hg_proc_hg_int32_t(proc, &in_data->aa);

    if (ret != HG_SUCCESS) {
    }

    return ret;
}

//hg_return_t my_in_proc_cb(hg_proc_t proc, void *data)
//{
//    return 0;
//}

hg_return_t my_out_proc_cb(hg_proc_t proc, void *data)
{
    hg_return_t ret;

    my_rpc_test_out_t *out_data;

    out_data = (my_rpc_test_out_t *) data;
    ret = hg_proc_hg_int32_t(proc, &out_data->bb);

    if (ret != HG_SUCCESS) {
    }

    return ret;
}

hg_return_t my_rpc_test_handler(hg_handle_t handle)
{
    my_rpc_test_in_t in_struct;
    my_rpc_test_out_t out_struct;
    struct my_rpc_test_state my_rpc_test_state_p;


    HG_Get_input(handle, &in_struct);
//    in_struct.aa = 50;
    fprintf(stdout, "got rpc request: ");
    fprintf(stdout, "input argument: %d\n", in_struct.aa);
//    my_rpc_test_state_p.cc = in_struct.aa + 1;
    out_struct.bb = in_struct.aa + 1;
    HG_Respond(handle, NULL, &my_rpc_test_state_p, &out_struct);
//    HG_Free_input(handle, in_struct);

    return 0;
}
