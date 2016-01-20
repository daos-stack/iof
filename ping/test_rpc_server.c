#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>

#include <mercury.h>

#include "my_rpc_common.h"

int main(int argc, char **argv)
{
    hg_id_t         my_rpc_id;
    na_class_t      *na_class = NULL;
    na_context_t    *na_context = NULL;
    hg_class_t      *hg_class = NULL;
    hg_context_t    *hg_context = NULL;
    hg_return_t ret;
    unsigned int act_count = 0;

//    char *uri       = "tcp://localhost:1234";
    char *uri       = "bmi+tcp://localhost:8888";

    na_class = NA_Initialize(uri, NA_TRUE);
    assert(na_class);
    na_context = NA_Context_create(na_class);
    assert(na_context);
    hg_class = HG_Init(na_class, na_context, NULL);
    assert(hg_class);
    hg_context = HG_Context_create(hg_class);
    assert(hg_context);

    my_rpc_id = HG_Register(hg_class, "rpc_test", my_in_proc_cb, my_out_proc_cb, my_rpc_test_handler);

    while (1) {
        do {
            ret = HG_Trigger(hg_class, hg_context, 0, 1, &act_count);
        } while (ret == HG_SUCCESS && act_count);
        HG_Progress(hg_class, hg_context, 100);
        if (act_count) break;
    }

    HG_Context_destroy(hg_context);
    HG_Finalize(hg_class);
    NA_Context_destroy(na_class, na_context);
    NA_Finalize(na_class);
}

