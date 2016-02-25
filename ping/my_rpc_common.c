#include "my_rpc_common.h"

hg_bool_t test_bulk_cb_done_g = HG_FALSE;
hg_return_t my_in_proc_cb(hg_proc_t proc, void *data)
{
    hg_return_t ret;

    my_rpc_test_in_t *in_data;

    in_data = (my_rpc_test_in_t *) data;
    ret = hg_proc_hg_int32_t(proc, &in_data->aa);
    ret = hg_proc_hg_bulk_t(proc, &in_data->bulk_handle);

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
    hg_bulk_t my_bulk_handle;
    struct hg_info *hgi;


    HG_Get_input(handle, &in_struct);
//    in_struct.aa = 50;
    fprintf(stdout, "got rpc request: ");
    fprintf(stdout, "input argument: %d\n", in_struct.aa);
//    my_rpc_test_state_p.cc = in_struct.aa + 1;
    out_struct.bb = in_struct.aa + 1;
    hgi = HG_Get_info(handle);
    my_rpc_test_state_p.size = 512;
    my_rpc_test_state_p.buffer = calloc(1, 512);
    HG_Bulk_create(hgi->hg_bulk_class, 1, &my_rpc_test_state_p.buffer,
            &my_rpc_test_state_p.size, HG_BULK_WRITE_ONLY,
            &my_bulk_handle);
    HG_Bulk_transfer(hgi->bulk_context, my_rpc_bulk_transfer_cb, NULL,
            HG_BULK_PULL, hgi->addr, in_struct.bulk_handle, 0, 
            my_bulk_handle, 0, my_rpc_test_state_p.size, HG_OP_ID_IGNORE);
    HG_Respond(handle, NULL, &my_rpc_test_state_p, &out_struct);
//    HG_Free_input(handle, in_struct);

    return 0;
}

hg_return_t  my_rpc_bulk_transfer_cb(const struct hg_bulk_cb_info *info)
{
    fprintf(stdout, "my_rpc_bulk_transfer_cb\n");
    test_bulk_cb_done_g = HG_TRUE;

    return 0;
}
