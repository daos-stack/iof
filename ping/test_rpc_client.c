#define _GNU_SOURCE
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>

#include <pmix.h>
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
    struct hg_info *hgi;
    pmix_proc_t myproc, proc;
    int rc;
    pmix_pdata_t *pdata;
    bool flag;
    pmix_info_t *info;

    my_rpc_test_state_p = malloc(sizeof(struct my_rpc_test_state));
    char *uri       = "bmi+tcp://localhost:8889";


    na_class = NA_Initialize(uri, NA_FALSE);
    assert(na_class);
    na_context = NA_Context_create(na_class);
    assert(na_context);
    hg_class = HG_Init(na_class, na_context, NULL);
    assert(hg_class);
    hg_context = HG_Context_create(hg_class);
    assert(hg_context);
    // ========= begin PMIx stuff
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %d\n",
                myproc.nspace, myproc.rank, rc);
        exit(0);
    }
    /* call fence to ensure the data is received */
    PMIX_PROC_CONSTRUCT(&proc);
    (void)strncpy(proc.nspace, myproc.nspace, PMIX_MAX_NSLEN);
    proc.rank = PMIX_RANK_WILDCARD;
    PMIX_INFO_CREATE(info, 1);
    flag = true;
    PMIX_INFO_LOAD(info, PMIX_COLLECT_DATA, &flag, PMIX_BOOL)
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, info, 1))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %d\n", myproc.nspace, myproc.rank, rc);
    }
    PMIX_INFO_FREE(info, 1);

    //lookup server uri
    PMIX_PDATA_CREATE(pdata, 1);
    (void)strncpy(pdata[0].key, "server-addr", PMIX_MAX_KEYLEN);
    if (PMIX_SUCCESS != (rc = PMIx_Lookup(pdata, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Lookup failed: %d\n", myproc.nspace, myproc.rank, rc);
    }
    NA_Addr_lookup_wait(na_class, pdata[0].value.data.string, &my_server_addr);
    PMIX_PDATA_FREE(pdata, 1);
    // ========= end PMIx stuff

    my_rpc_id = HG_Register(hg_class, "rpc_test", my_in_proc_cb, my_out_proc_cb, my_rpc_test_handler);
    HG_Create(hg_class, hg_context, my_server_addr, my_rpc_id, &my_hg_handle);
    hgi = HG_Get_info(my_hg_handle);
    my_rpc_test_state_p->size = 512;
    my_rpc_test_state_p->buffer = calloc(1, 512);
    HG_Bulk_create(hgi->hg_bulk_class, 1, &my_rpc_test_state_p->buffer,
            &my_rpc_test_state_p->size, HG_BULK_READ_ONLY,
            &in_struct.bulk_handle);
    my_rpc_test_state_p->cc = 18;
    in_struct.aa = 19;
    HG_Forward(my_hg_handle, my_rpc_test_cb, my_rpc_test_state_p, &in_struct);

    while (1) {
        do {
            ret = HG_Trigger(hg_class, hg_context, 0, 1, &act_count);
        } while (ret == HG_SUCCESS && act_count);
        HG_Progress(hg_class, hg_context, 100);
        if (act_count) break;
    }
    if (PMIX_SUCCESS != (rc = PMIx_Finalize())) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Finalize failed: %d\n",
                myproc.nspace, myproc.rank, rc);
    }
    HG_Context_destroy(hg_context);
    HG_Finalize(hg_class);
    NA_Context_destroy(na_class, na_context);
    NA_Finalize(na_class);

    return 0;
}


hg_return_t my_rpc_test_cb(const struct hg_cb_info *info)
{
    my_rpc_test_out_t out_struct;

    HG_Get_output(info->handle, &out_struct);
    fprintf(stdout, "rpc_test finished on remote node, ");
    fprintf(stdout, "return value: %d\n", out_struct.bb);

    return 0;
}
