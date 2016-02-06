#include <sys/stat.h>
#include "server_backend.h"
#include "rpc_common.h"

#ifndef RPC_HANDLER_H
#define RPC_HANDLER_H

#define RW_LOCK pthread_rwlock_t
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

/*Gettattr () */
struct getattr_in_t {
	const char *name;
};

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

struct mkdir_out_t {
	uint64_t error_code;
};

/*rmdir*/
struct rmdir_in_t {
	const char *name;
};

struct rmdir_out_t {
	uint64_t error_code;
};

/*symlink*/
struct symlink_in_t {
	const char *name;
	const char *dst;
};

struct symlink_out_t {
	uint64_t error_code;
};

/*readlink()*/
struct readlink_in_t {
	const char *name;
};

struct readlink_out_t {
	char *dst;
	uint64_t error_code;
};

/*unlink()*/
struct unlink_in_t {
	const char *name;
};

struct unlink_out_t {
	uint64_t error_code;
};

hg_return_t getattr_in_proc_cb(hg_proc_t proc, void *data);
hg_return_t getattr_out_proc_cb(hg_proc_t proc, void *data);
hg_id_t getattr_register(hg_class_t *);

hg_id_t readdir_register(hg_class_t *);
hg_return_t readdir_in_proc_cb(hg_proc_t, void *data);
hg_return_t readdir_out_proc_cb(hg_proc_t proc, void *data);

hg_id_t mkdir_register(hg_class_t *);
hg_return_t mkdir_in_proc_cb(hg_proc_t, void *data);
hg_return_t mkdir_out_proc_cb(hg_proc_t, void *data);

hg_id_t rmdir_register(hg_class_t *);
hg_return_t rmdir_in_proc_cb(hg_proc_t proc, void *data);
hg_return_t rmdir_out_proc_cb(hg_proc_t proc, void *data);

hg_id_t symlink_register(hg_class_t *);
hg_return_t symlink_in_proc_cb(hg_proc_t proc, void *data);
hg_return_t symlink_out_proc_cb(hg_proc_t proc, void *data);

hg_id_t readlink_register(hg_class_t *);
hg_return_t readlink_in_proc_cb(hg_proc_t proc, void *data);
hg_return_t readlink_out_proc_cb(hg_proc_t proc, void *data);

hg_id_t unlink_register(hg_class_t *);
hg_return_t unlink_in_proc_cb(hg_proc_t proc, void *data);
hg_return_t unlink_out_proc_cb(hg_proc_t proc, void *data);
#endif
