#include <assert.h>
#include<unistd.h>
#include<stdio.h>
#include "rpc_common.h"
#include "rpc_handler.h"
#include "process_set.h"

/* Will be the wrapper on top of the file system that will register
 * and handle the RPC. It will execute indefinitely.
 */

/* This function will basically create ID's and register all function calls on
 * the client. All register functions are defined in client_handler.c
 */
struct rpc_id rpc_id;

void server_init(hg_class_t *rpc_class)
{
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
	na_class_t *na_class = NULL;
	hg_class_t *rpc_class = NULL;
	char *uri;
	struct mcl_set *set;
	struct mcl_state *proc_state;
	int is_service = 1;
	char *name_of_set = "server";
	filesystem_init();
	proc_state = mcl_init(&uri);
	rpc_class = engine_init(NA_TRUE, uri, 1, &na_class);
	server_init(rpc_class);
	mcl_startup(proc_state, name_of_set, is_service, &set);
	while (1)
		sleep(1);
	/*useless at this point*/
	mcl_set_free(na_class, set);
	mcl_finalize(proc_state);
	return 0;

}
