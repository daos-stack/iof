#include <sys/stat.h>
#include "server_backend.h"
#include "rpc_common.h"

#ifndef RPC_HANDLER_H
#define RPC_HANDLER_H

struct rpc_id {
	hg_id_t getattr_id;
	hg_id_t readdir_id;
	hg_id_t mkdir_id;
	hg_id_t rmdir_id;
	hg_id_t symlink_id;
	hg_id_t readlink_id;
	hg_id_t unlink_id;
};

struct rpc_handle {
	hg_handle_t getattr_handle;
	hg_handle_t readdir_handle;
	hg_handle_t mkdir_handle;
	hg_handle_t rmdir_handle;
	hg_handle_t symlink_handle;
	hg_handle_t readlink_handle;
	hg_handle_t unlink_handle;
};

/* Common struct for RPC types which send a single string */
struct rpc_request_string {
	const char *name;
};

/* Common struct for RPC types which reply with a single string */
struct rpc_reply_basic {
	uint64_t error_code;
};

/* Types for RPCs which have custom parameters */

struct getattr_out_t {
	struct stat stbuf;
	uint64_t error_code;
};

/*readdir()*/
struct readdir_in_t {
	const char *dir_name;
	uint64_t offset;
};

struct readdir_out_t {
	char name[255];
	struct stat stat;
	uint64_t error_code;
	uint64_t complete;
};

/*mkdir()*/
struct mkdir_in_t {
	const char *name;
	mode_t mode;
};

/*symlink*/
struct symlink_in_t {
	const char *name;
	const char *dst;
};

struct readlink_out_t {
	char *dst;
	uint64_t error_code;
};

hg_return_t getattr_out_proc_cb(hg_proc_t proc, void *data);
hg_id_t getattr_register(hg_class_t *);

hg_id_t readdir_register(hg_class_t *);
hg_return_t readdir_in_proc_cb(hg_proc_t, void *data);
hg_return_t readdir_out_proc_cb(hg_proc_t proc, void *data);

hg_id_t mkdir_register(hg_class_t *);
hg_return_t mkdir_in_proc_cb(hg_proc_t, void *data);

hg_id_t rmdir_register(hg_class_t *);

hg_id_t symlink_register(hg_class_t *);
hg_return_t symlink_in_proc_cb(hg_proc_t proc, void *data);

hg_id_t readlink_register(hg_class_t *);
hg_return_t readlink_out_proc_cb(hg_proc_t proc, void *data);

hg_id_t unlink_register(hg_class_t *);
#endif
