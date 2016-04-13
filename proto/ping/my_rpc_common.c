#include "my_rpc_common.h"
#include "proto_common.h"

hg_bool_t test_bulk_cb_done_g = HG_FALSE;

hg_return_t my_in_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;

	struct my_rpc_test_in_t *in_data;

	printf("Packing data\n");

	in_data = (struct my_rpc_test_in_t *) data;
	ret = hg_proc_hg_int32_t(proc, &in_data->aa);
	ret = hg_proc_hg_bulk_t(proc, &in_data->bulk_handle);

	return ret;
}

hg_return_t my_out_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;

	struct my_rpc_test_out_t *out_data;

	printf("Unpacking data\n");

	out_data = (struct my_rpc_test_out_t *) data;
	ret = hg_proc_hg_int32_t(proc, &out_data->bb);

	return ret;
}

hg_return_t my_rpc_bulk_transfer_cb(const struct hg_cb_info *info)
{
	fprintf(stdout, "my_rpc_bulk_transfer_cb\n");
	HG_Bulk_free(info->arg);
	test_bulk_cb_done_g = HG_TRUE;

	return 0;
}

hg_return_t my_test_rpc_handle_complete(const struct hg_cb_info *info)
{
	free(info->arg);
	return 0;
}

hg_return_t my_rpc_test_handler(hg_handle_t handle)
{
	struct my_rpc_test_in_t in_struct;
	struct my_rpc_test_out_t out_struct;
	struct my_rpc_test_state my_rpc_test_state_p;
	hg_bulk_t my_bulk_handle;
	struct hg_info *hgi;

	HG_Get_input(handle, &in_struct);
	fprintf(stdout, "got rpc request: ");
	fprintf(stdout, "input argument: %d\n", in_struct.aa);
	out_struct.bb = in_struct.aa + 1;
	hgi = HG_Get_info(handle);
	my_rpc_test_state_p.size = 512;
	my_rpc_test_state_p.buffer = calloc(1, 512);
	HG_Bulk_create(hgi->hg_class, 1, &my_rpc_test_state_p.buffer,
		       &my_rpc_test_state_p.size, HG_BULK_WRITE_ONLY,
		       &my_bulk_handle);
	HG_Bulk_transfer(hgi->context, my_rpc_bulk_transfer_cb, my_bulk_handle,
			 HG_BULK_PULL, hgi->addr, in_struct.bulk_handle, 0,
			 my_bulk_handle, 0, my_rpc_test_state_p.size,
			 HG_OP_ID_IGNORE);
	HG_Respond(handle, my_test_rpc_handle_complete,
		   my_rpc_test_state_p.buffer, &out_struct);
	HG_Free_input(handle, &in_struct);

	return 0;
}
