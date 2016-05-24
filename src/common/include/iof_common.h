#ifndef IOF_COMMON_H
#define IOF_COMMON_H
#include <mercury.h>
#include <mercury_proc.h>
#include <mercury_proc_string.h>
#include <process_set.h>

struct iof_fs_info {
	/*Associated mount point*/
	char mnt[80];
	/*id of filesystem*/
	uint64_t id;
	/*mode of projection, set to 0 for private mode*/
	uint8_t mode;
};

struct iof_psr_query {
	struct iof_fs_info *list;
	uint64_t num;
};

/* This currently returns dummy data */
hg_return_t iof_query_handler(hg_handle_t handle);
hg_return_t iof_query_out_proc_cb(hg_proc_t proc, void *data);

#endif
