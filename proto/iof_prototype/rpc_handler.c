#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include "rpc_common.h"
#include "rpc_handler.h"
#include "server_backend.h"
#include <mercury_proc_string.h>
#include <errno.h>

static hg_return_t getattr_handler(hg_handle_t handle)
{
	struct getattr_in_t in;
	struct getattr_out_t out = {0};
	uint64_t ret;
	struct stat *buf;

	ret = HG_Get_input(handle, &in);
	assert(ret == HG_SUCCESS);
	printf("got RPC\n");

	buf = &out.stbuf;
	/* send arguments to the local filesystem */
	printf("Calling getattr %s %p\n", in.name, (void *)buf);
	out.error_code = iof_getattr(in.name, buf);
	ret = HG_Free_input(handle, &in);
	assert(ret == HG_SUCCESS);

	ret = HG_Respond(handle, NULL, NULL, &out);
	assert(ret == HG_SUCCESS);
	return ret;
}

static hg_return_t readdir_handler(hg_handle_t handle)
{
	struct readdir_in_t in;
	struct readdir_out_t out = {0};
	uint64_t ret;

	ret = HG_Get_input(handle, &in);
	assert(ret == HG_SUCCESS);
	/* in has the input args now */
	printf("got  RPC\n");

	out.complete =
	    iof_readdir(out.name, in.dir_name, &out.error_code, in.offset,
			&out.stat);
	ret = HG_Free_input(handle, &in);
	assert(ret == HG_SUCCESS);
	ret = HG_Respond(handle, NULL, NULL, &out);
	assert(ret == HG_SUCCESS);
	return ret;
}

static hg_return_t mkdir_handler(hg_handle_t handle)
{
	struct mkdir_in_t in;
	struct mkdir_out_t out = {0};
	hg_return_t ret;

	ret = HG_Get_input(handle, &in);
	assert(ret == HG_SUCCESS);

	out.error_code = iof_mkdir(in.name, in.mode);
	ret = HG_Free_input(handle, &in);
	assert(ret == HG_SUCCESS);
	ret = HG_Respond(handle, NULL, NULL, &out);
	assert(ret == HG_SUCCESS);

	return ret;
}

static hg_return_t rmdir_handler(hg_handle_t handle)
{
	struct rmdir_in_t in;
	struct rmdir_out_t out = {0};
	int ret;

	ret = HG_Get_input(handle, &in);
	assert(ret == HG_SUCCESS);

	out.error_code = iof_rmdir(in.name);
	ret = HG_Free_input(handle, &in);
	assert(ret == HG_SUCCESS);
	ret = HG_Respond(handle, NULL, NULL, &out);
	assert(ret == HG_SUCCESS);

	return ret;
}

static hg_return_t symlink_handler(hg_handle_t handle)
{
	struct symlink_in_t in;
	struct symlink_out_t out = {0};
	int ret;

	ret = HG_Get_input(handle, &in);
	assert(ret == HG_SUCCESS);

	out.error_code = iof_symlink(in.dst, in.name);
	ret = HG_Free_input(handle, &in);
	assert(ret == HG_SUCCESS);

	ret = HG_Respond(handle, NULL, NULL, &out);
	assert(ret == HG_SUCCESS);

	return ret;
}

static hg_return_t readlink_handler(hg_handle_t handle)
{
	struct readlink_in_t in;
	struct readlink_out_t out = {0};
	int ret;

	ret = HG_Get_input(handle, &in);
	assert(ret == HG_SUCCESS);

	out.error_code = iof_readlink(in.name, &out.dst);

	ret = HG_Free_input(handle, &in);
	assert(ret == HG_SUCCESS);

	ret = HG_Respond(handle, NULL, NULL, &out);
	assert(ret == HG_SUCCESS);

	return ret;
}

static hg_return_t unlink_handler(hg_handle_t handle)
{
	struct unlink_in_t in;
	struct unlink_out_t out = {0};
	int ret;

	ret = HG_Get_input(handle, &in);
	assert(ret == HG_SUCCESS);

	out.error_code = iof_unlink(in.name);

	ret = HG_Free_input(handle, &in);
	assert(ret == HG_SUCCESS);

	ret = HG_Respond(handle, NULL, NULL, &out);
	assert(ret == HG_SUCCESS);

	return ret;
}

/* registration functions for client/server */
hg_id_t getattr_register(hg_class_t *rpc_class)
{
	hg_id_t tmp;

	tmp = HG_Register_name(rpc_class, "getattr", getattr_in_proc_cb,
			       getattr_out_proc_cb, getattr_handler);
	return tmp;
}

hg_id_t readdir_register(hg_class_t *rpc_class)
{
	hg_id_t tmp;

	tmp = HG_Register_name(rpc_class, "readdir", readdir_in_proc_cb,
			       readdir_out_proc_cb, readdir_handler);
	return tmp;
}

hg_id_t mkdir_register(hg_class_t *rpc_class)
{
	hg_id_t tmp;

	tmp =
	    HG_Register_name(rpc_class, "mkdir", mkdir_in_proc_cb,
			     mkdir_out_proc_cb, mkdir_handler);
	return tmp;
}

hg_id_t rmdir_register(hg_class_t *rpc_class)
{
	hg_id_t tmp;

	tmp =
	    HG_Register_name(rpc_class, "rmdir", rmdir_in_proc_cb,
			     rmdir_out_proc_cb, rmdir_handler);
	return tmp;
}

hg_id_t symlink_register(hg_class_t *rpc_class)
{
	hg_id_t tmp;

	tmp =
	    HG_Register_name(rpc_class, "symlink", symlink_in_proc_cb,
			     symlink_out_proc_cb, symlink_handler);
	return tmp;
}

hg_id_t readlink_register(hg_class_t *rpc_class)
{
	hg_id_t tmp;

	tmp =
	    HG_Register_name(rpc_class, "readlink", readlink_in_proc_cb,
			     readlink_out_proc_cb, readlink_handler);
	return tmp;
}

hg_id_t unlink_register(hg_class_t *rpc_class)
{
	hg_id_t tmp;

	tmp =
	    HG_Register_name(rpc_class, "unlink", unlink_in_proc_cb,
			     unlink_out_proc_cb, unlink_handler);
	return tmp;
}

/* Serialization function for sending arguments*/
hg_return_t getattr_in_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct getattr_in_t *struct_data = (struct getattr_in_t *)data;

	ret = hg_proc_hg_const_string_t(proc, &struct_data->name);
	assert(ret == HG_SUCCESS);
	return ret;
}

hg_return_t getattr_out_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct getattr_out_t *struct_data = (struct getattr_out_t *)data;

	ret =
	    hg_proc_raw(proc, &struct_data->stbuf, sizeof(struct_data->stbuf));

	assert(ret == HG_SUCCESS);

	ret = hg_proc_hg_uint64_t(proc, &struct_data->error_code);
	assert(ret == HG_SUCCESS);
	return ret;
}

hg_return_t readdir_in_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct readdir_in_t *struct_data = (struct readdir_in_t *)data;
	/* Dir_name */
	ret = hg_proc_hg_const_string_t(proc, &struct_data->dir_name);
	assert(ret == HG_SUCCESS);
	/*offset */
	ret = hg_proc_hg_uint64_t(proc, &struct_data->offset);
	assert(ret == HG_SUCCESS);
	return ret;
}

hg_return_t readdir_out_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct readdir_out_t *struct_data = (struct readdir_out_t *)data;
	/* name */
	ret = hg_proc_raw(proc, &struct_data->name, sizeof(struct_data->name));
	assert(ret == HG_SUCCESS);
	ret = hg_proc_hg_uint64_t(proc, &struct_data->error_code);
	assert(ret == HG_SUCCESS);
	ret = hg_proc_hg_uint64_t(proc, &struct_data->complete);
	assert(ret == HG_SUCCESS);
	ret = hg_proc_raw(proc, &struct_data->stat, sizeof(struct stat));
	assert(ret == HG_SUCCESS);
	return ret;
}

hg_return_t mkdir_in_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct mkdir_in_t *struct_data = (struct mkdir_in_t *)data;

	ret = hg_proc_hg_const_string_t(proc, &struct_data->name);
	assert(ret == HG_SUCCESS);

	ret = hg_proc_raw(proc, &struct_data->mode, sizeof(struct_data->mode));
	assert(ret == HG_SUCCESS);

	return ret;
}

hg_return_t mkdir_out_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct mkdir_out_t *struct_data = (struct mkdir_out_t *)data;

	ret = hg_proc_hg_uint64_t(proc, &struct_data->error_code);
	assert(ret == HG_SUCCESS);

	return ret;
}

hg_return_t rmdir_in_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct rmdir_in_t *struct_data = (struct rmdir_in_t *)data;

	ret = hg_proc_hg_const_string_t(proc, &struct_data->name);
	assert(ret == HG_SUCCESS);

	return ret;
}

hg_return_t rmdir_out_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct rmdir_out_t *struct_data = (struct rmdir_out_t *)data;

	ret = hg_proc_hg_uint64_t(proc, &struct_data->error_code);
	assert(ret == HG_SUCCESS);

	return ret;
}

hg_return_t symlink_in_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct symlink_in_t *struct_data = (struct symlink_in_t *)data;

	ret = hg_proc_hg_const_string_t(proc, &struct_data->name);
	assert(ret == HG_SUCCESS);

	ret = hg_proc_hg_const_string_t(proc, &struct_data->dst);
	assert(ret == HG_SUCCESS);
	return ret;
}

hg_return_t symlink_out_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct symlink_out_t *struct_data = (struct symlink_out_t *)data;

	ret = hg_proc_hg_uint64_t(proc, &struct_data->error_code);
	assert(ret == HG_SUCCESS);

	return ret;
}

hg_return_t readlink_in_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct readlink_in_t *struct_data = (struct readlink_in_t *)data;

	ret = hg_proc_hg_const_string_t(proc, &struct_data->name);
	assert(ret == HG_SUCCESS);

	return ret;
}

hg_return_t readlink_out_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct readlink_out_t *struct_data = (struct readlink_out_t *)data;

	ret = hg_proc_hg_string_t(proc, &struct_data->dst);
	assert(ret == HG_SUCCESS);

	ret = hg_proc_hg_uint64_t(proc, &struct_data->error_code);
	assert(ret == HG_SUCCESS);

	return ret;
}

hg_return_t unlink_out_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct unlink_out_t *struct_data = (struct unlink_out_t *)data;

	ret = hg_proc_hg_uint64_t(proc, &struct_data->error_code);
	assert(ret == HG_SUCCESS);

	return ret;
}

hg_return_t unlink_in_proc_cb(hg_proc_t proc, void *data)
{
	hg_return_t ret;
	struct unlink_in_t *struct_data = (struct unlink_in_t *)data;

	ret = hg_proc_hg_const_string_t(proc, &struct_data->name);
	assert(ret == HG_SUCCESS);

	return ret;
}
