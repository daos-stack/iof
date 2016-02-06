#include <assert.h>
#include<unistd.h>
#include<stdio.h>
#include "rpc_common.h"
#include "rpc_handler.h"

/* Will be the wrapper on top of the file system that will register
 * and handle the RPC. It will execute indefinitely.
 */

/* This function will basically create ID's and register all function calls on
 * the client. All register functions are defined in client_handler.c
 */
struct rpc_id rpc_id;

void server_init(void)
{
	hg_class_t *rpc_class = engine_init(NA_TRUE, "tcp://localhost:1234", 1);

	rpc_id.readdir_id = readdir_register(rpc_class);

	rpc_id.getattr_id = getattr_register(rpc_class);

	rpc_id.mkdir_id = mkdir_register(rpc_class);

	rpc_id.rmdir_id = rmdir_register(rpc_class);

	rpc_id.symlink_id = symlink_register(rpc_class);

	rpc_id.readlink_id = readlink_register(rpc_class);

	rpc_id.unlink_id = unlink_register(rpc_class);
}

int main(int argc, char **argv)
{
	filesystem_init();
	server_init();
	while (1)
		sleep(1);

	engine_finalize();
	return 0;
}
