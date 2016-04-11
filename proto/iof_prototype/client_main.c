#define FUSE_USE_VERSION 30

#include <fuse.h>
#include <fuse/fuse_lowlevel.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <inttypes.h>
#include <signal.h>

#include "rpc_handler.h"
#include "rpc_common.h"
#include "server_backend.h"
#include "process_set.h"

struct rpc_handle rpc_handle;
struct rpc_id rpc_id;

struct fuse *fuse;

struct getattr_r_t {
	struct stat *stbuf;
	int done;
	int err_code;
};

struct readdir_r_t {
	uint64_t complete;
	uint64_t err_code;
	char name[255];
	struct stat stat;
	int done;
};

struct rpc_cb_basic {
	uint64_t err_code;
	int done;
};

struct readlink_r_t {
	char *dst;
	uint64_t err_code;
	int done;
};

void client_init(hg_class_t *rpc_class, na_addr_t svr_addr)
{
	/*getattr() */
	rpc_id.getattr_id = getattr_register(rpc_class);
	engine_create_handle(svr_addr, rpc_id.getattr_id,
			     &rpc_handle.getattr_handle);
	/*readdir() */
	rpc_id.readdir_id = readdir_register(rpc_class);
	engine_create_handle(svr_addr, rpc_id.readdir_id,
			     &rpc_handle.readdir_handle);
	/*mkdir () */
	rpc_id.mkdir_id = mkdir_register(rpc_class);
	engine_create_handle(svr_addr, rpc_id.mkdir_id,
			     &rpc_handle.mkdir_handle);
	/*rmdir() */
	rpc_id.rmdir_id = rmdir_register(rpc_class);
	engine_create_handle(svr_addr, rpc_id.rmdir_id,
			     &rpc_handle.rmdir_handle);
	/*symlink() */
	rpc_id.symlink_id = symlink_register(rpc_class);
	engine_create_handle(svr_addr, rpc_id.symlink_id,
			     &rpc_handle.symlink_handle);
	/*readlink() */
	rpc_id.readlink_id = readlink_register(rpc_class);
	engine_create_handle(svr_addr, rpc_id.readlink_id,
			     &rpc_handle.readlink_handle);

	rpc_id.unlink_id = unlink_register(rpc_class);
	engine_create_handle(svr_addr, rpc_id.unlink_id,
			     &rpc_handle.unlink_handle);
}

static hg_return_t getattr_callback(const struct hg_cb_info *info)
{
	struct getattr_r_t *reply;
	struct getattr_out_t out = {0};
	uint64_t ret;

	reply = info->arg;
	ret = HG_Get_output(info->info.forward.handle, &out);
	assert(ret == HG_SUCCESS);

	reply->err_code = out.error_code;
	memcpy(reply->stbuf, &out.stbuf, sizeof(struct stat));
	ret = HG_Free_output(info->info.forward.handle, &out);
	assert(ret == HG_SUCCESS);

	reply->done = 1;

	return HG_SUCCESS;
}

static int fs_getattr(const char *name, struct stat *stbuf)
{
	uint64_t ret;
	struct rpc_request_string in;
	struct getattr_r_t reply = {0};

	in.name = name;
	reply.stbuf = stbuf;

	/* Send RPC */
	printf("Reply is at %p\n", (void *)&reply);
	ret =
	    HG_Forward(rpc_handle.getattr_handle, getattr_callback, &reply,
		       &in);
	assert(ret == 0);

	while (reply.done == 0) {
		ret = engine_progress(&reply.done);
		if (ret != HG_SUCCESS) {
			fuse_session_exit(fuse_get_session(fuse));
			return -EINTR;
		}
	}

	return reply.err_code;
}

static hg_return_t readdir_callback(const struct hg_cb_info *info)
{
	struct readdir_out_t out = {0};
	struct readdir_r_t *reply = info->arg;
	uint64_t ret;

	ret = HG_Get_output(info->info.forward.handle, &out);
	assert(ret == HG_SUCCESS);

	reply->err_code = out.error_code;
	printf("%s\n", out.name);
	strcpy(reply->name, out.name);
	memcpy(&reply->stat, &out.stat, sizeof(out.stat));
	reply->complete = out.complete;
	ret = HG_Free_output(info->info.forward.handle, &out);
	assert(ret == HG_SUCCESS);

	printf("\n successful");

	reply->done = 1;

	return HG_SUCCESS;
}

static int
fs_readdir(const char *dir_name, void *buf, fuse_fill_dir_t filler,
	   off_t offset, struct fuse_file_info *fi)
{
	struct readdir_r_t reply = {0};
	uint64_t ret;
	struct readdir_in_t in;
	/* pack arguments */
	in.offset = 0;
	in.dir_name = dir_name;
	do {
		printf("Calling readdir rpc %s %" PRIu64 "\n",
		       in.dir_name, in.offset);
		ret =
		    HG_Forward(rpc_handle.readdir_handle, readdir_callback,
			       &reply, &in);
		assert(ret == 0);
		while (!reply.done) {
			ret = engine_progress(&reply.done);
			if (ret != HG_SUCCESS) {
				fuse_session_exit(fuse_get_session(fuse));
				return -EINTR;
			}
		}
		if (reply.err_code != 0)
			return reply.err_code;
		printf("Calling filler %s\n", reply.name);
		filler(buf, reply.name, &reply.stat, 0);
		in.offset++;
		reply.done = 0;
	} while (!reply.complete);

	printf("Finished readdir\n");
	return 0;
}

static hg_return_t basic_callback(const struct hg_cb_info *info)
{
	struct rpc_reply_basic out = {0};
	struct rpc_cb_basic *reply;
	int ret;

	reply = info->arg;
	ret = HG_Get_output(info->info.forward.handle, &out);
	assert(ret == HG_SUCCESS);

	reply->err_code = out.error_code;
	ret = HG_Free_output(info->info.forward.handle, &out);
	assert(ret == HG_SUCCESS);

	reply->done = 1;

	return HG_SUCCESS;
}

static int fs_mkdir(const char *name, mode_t mode)
{
	struct mkdir_in_t in;
	struct rpc_cb_basic reply = {0};
	int ret;

	in.name = name;
	in.mode = mode;

	ret = HG_Forward(rpc_handle.mkdir_handle, basic_callback, &reply, &in);
	assert(ret == 0);

	while (reply.done == 0) {
		ret = engine_progress(&reply.done);
		if (ret != HG_SUCCESS) {
			fuse_session_exit(fuse_get_session(fuse));
			return -EINTR;
		}
	}

	return reply.err_code;
}


static int fs_rmdir(const char *name)
{
	struct rpc_cb_basic reply = {0};
	struct rpc_request_string in;
	int ret;

	in.name = name;
	ret = HG_Forward(rpc_handle.rmdir_handle, basic_callback, &reply, &in);
	assert(ret == 0);
	while (reply.done == 0) {
		ret = engine_progress(&reply.done);
		if (ret != HG_SUCCESS) {
			fuse_session_exit(fuse_get_session(fuse));
			return -EINTR;
		}
	}

	return reply.err_code;
}

static int fs_symlink(const char *dst, const char *name)
{
	struct symlink_in_t in;
	struct rpc_cb_basic reply = {0};
	int ret;

	in.name = name;
	in.dst = dst;

	ret =
	    HG_Forward(rpc_handle.symlink_handle, basic_callback, &reply, &in);
	assert(ret == 0);
	while (reply.done == 0) {
		ret = engine_progress(&reply.done);
		if (ret != HG_SUCCESS) {
			fuse_session_exit(fuse_get_session(fuse));
			return -EINTR;
		}
	}

	return reply.err_code;
}

hg_return_t readlink_callback(const struct hg_cb_info *info)
{
	struct readlink_r_t *reply;
	struct readlink_out_t out = {0};
	int ret;

	reply = info->arg;

	ret = HG_Get_output(info->info.forward.handle, &out);
	assert(ret == HG_SUCCESS);

	reply->err_code = out.error_code;
	strncpy(reply->dst, out.dst, strlen(out.dst)+1);

	ret = HG_Free_output(info->info.forward.handle, &out);
	assert(ret == 0);

	reply->done = 1;

	return HG_SUCCESS;
}

static int fs_readlink(const char *name, char *dst, size_t length)
{
	struct rpc_request_string in;
	struct readlink_r_t reply = {0};
	int ret;

	in.name = name;
	reply.dst = dst;

	ret =
	    HG_Forward(rpc_handle.readlink_handle, readlink_callback, &reply,
		       &in);
	assert(ret == 0);

	while (reply.done == 0) {
		ret = engine_progress(&reply.done);
		if (ret != HG_SUCCESS) {
			fuse_session_exit(fuse_get_session(fuse));
			return -EINTR;
		}
	}

	return reply.err_code;
}

hg_return_t unlink_callback(const struct hg_cb_info *info)
{
	struct rpc_cb_basic *reply;
	struct rpc_reply_basic out;
	int ret;

	reply = info->arg;
	ret = HG_Get_output(info->info.forward.handle, &out);
	assert(ret == 0);

	reply->err_code = out.error_code;

	ret = HG_Free_output(info->info.forward.handle, &out);
	assert(ret == 0);

	reply->done = 1;

	return HG_SUCCESS;
}

static int fs_unlink(const char *name)
{
	struct rpc_request_string in;
	struct rpc_cb_basic reply = {0};
	int ret;

	in.name = name;
	ret =
	    HG_Forward(rpc_handle.unlink_handle, unlink_callback, &reply, &in);
	assert(ret == 0);

	while (reply.done == 0)
		ret = engine_progress(&reply.done);
	if (ret != HG_SUCCESS) {
		fuse_session_exit(fuse_get_session(fuse));
		return -EINTR;
	}

	return reply.err_code;
}

static struct fuse_operations op = {
	.readdir = fs_readdir,
	.getattr = fs_getattr,
	.mkdir = fs_mkdir,
	.rmdir = fs_rmdir,
	.symlink = fs_symlink,
	.readlink = fs_readlink,
	.unlink = fs_unlink,
};

int main(int argc, char **argv)
{
	na_class_t *na_class = NULL;
	hg_class_t *rpc_class = NULL;
	char *uri;
	struct mcl_set *set;
	struct mcl_state *proc_state;
	struct fuse_chan *ch;
	char *mountpoint;
	int foreground, multithreaded;
	int res;
	int is_service = 0;
	char *name_of_set = "client";
	char *name_of_target_set = "server";
	struct mcl_set *dest_set;
	na_addr_t dest_addr;
	int ret;
	struct fuse_args args;

	proc_state = mcl_init(&uri);
	rpc_class = engine_init(NA_FALSE, uri, 0, &na_class);
	mcl_startup(proc_state, na_class, name_of_set, is_service, &set);
	ret = mcl_attach(proc_state, name_of_target_set, &dest_set);
	if (ret != MCL_SUCCESS) {
		fprintf(stderr, "attach failed\n");
		return 0;
	}
	mcl_lookup(dest_set, 0, na_class, &dest_addr);
	client_init(rpc_class, dest_addr);

	args.argc = argc;
	args.argv = argv;
	args.allocated = 0;

	res =
	    fuse_parse_cmdline(&args, &mountpoint, &multithreaded, &foreground);
	if (res == -1)
		return 0;
	ch = fuse_mount(mountpoint, &args);
	if (!ch)
		fuse_opt_free_args(&args);
	fuse = fuse_new(ch, &args, &op, sizeof(op), NULL);
	fuse_opt_free_args(&args);

	res = fuse_set_signal_handlers(fuse_get_session(fuse));
	assert(res != -1);
	/* This needs to change to go to unmount instead
	 * possibly using a goto statement???
	 */
	if (multithreaded)
		res = fuse_loop_mt(fuse);
	else
		res = fuse_loop(fuse);

	/*shutdown before returning */
	fuse_teardown(fuse, mountpoint);
	mcl_set_free(na_class, dest_set);
	NA_Finalize(na_class);
	mcl_finalize(proc_state);
	return 0;
}
