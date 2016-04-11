#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>

#include "mercury.h"
#include "include/process_set.h"
#include "test_ps_common.h"

int main(int argc, char **argv)
{
	na_class_t *na_class = NULL;
	char *uri;
	struct mcl_set *set;
	struct mcl_state *proc_state;
	int is_service = 1;
	int should_attach = 0;
	char name_of_set[MCL_NAME_LEN_MAX];
	char name_of_target_set[MCL_NAME_LEN_MAX];
	int ii;

	snprintf(name_of_set, MCL_NAME_LEN_MAX, "%s", "first_set");
	for (ii = 1; ii < argc; ii++) {
		if (strcmp(argv[ii], "--name") == 0) {
			strncpy(name_of_set, argv[ii + 1], MCL_NAME_LEN_MAX);
			ii++;
		}
		if (strcmp(argv[ii], "--is_service") == 0) {
			is_service = atoi(argv[ii + 1]);
			ii++;
		}
		if (strcmp(argv[ii], "--attach_to") == 0) {
			strncpy(name_of_target_set, argv[ii + 1],
				MCL_NAME_LEN_MAX);
			should_attach = 1;
			ii++;
		}
	}
	proc_state = mcl_init(&uri);
	if (proc_state == NULL) {
		fprintf(stderr, "mcl_init() failed.\n");
		return -1;
	}
	na_class = NA_Initialize(uri, NA_TRUE);
	assert(na_class != NULL);
	free(uri);
	if (is_service)
		test_create_progress_thread(na_class);
	mcl_startup(proc_state, na_class, name_of_set, is_service, &set);
	fprintf(stderr, "name %s size %d rank %d is_local %d is_service %d\n",
			set->name, set->size, set->self, set->is_local,
			set->is_service);
	if (should_attach == 1) {
		struct mcl_set *dest_set;
		na_addr_t dest_addr;
		int ret;
		int ii;

		ret = mcl_attach(proc_state, name_of_target_set, &dest_set);
		if (ret != MCL_SUCCESS)
			fprintf(stderr, "attach failed\n");
		fprintf(stderr, "dest: name %s size %d rank %d is_local %d is_service %d\n",
				dest_set->name, dest_set->size, dest_set->self,
				dest_set->is_local, dest_set->is_service);
		for (ii = 0; ii < dest_set->size; ii++) {
			ret = mcl_lookup(dest_set, ii, na_class, &dest_addr);
			printf("Remote address (%d) %p\n", ret, dest_addr);
		}
		ret = mcl_lookup(dest_set, dest_set->size + 100, na_class,
				 &dest_addr);
		mcl_set_free(na_class, dest_set);
	}

	mcl_set_free(na_class, set);
	mcl_finalize(proc_state);
	if (is_service)
		test_destroy_progress_thread();
	NA_Finalize(na_class);

	return 0;
}
