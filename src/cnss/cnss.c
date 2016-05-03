
#include <stdio.h>
#include "version.h"
#include <mercury.h>
#include <process_set.h>

int main(void)
{
	char *uri;
	struct mcl_set *set;
	struct mcl_state *proc_state;
	na_class_t *na_class = NULL;

	char *version = iof_get_version();

	printf("CNSS version: %s %d\n", version, gv());

	proc_state = mcl_init(&uri);
	if (proc_state == NULL) {
		fprintf(stderr, "mcl_init() failed.\n");
		return 1;
	}
	na_class = NA_Initialize(uri, NA_TRUE);

	free(uri);

	mcl_startup(proc_state, na_class, "cnss", 0, &set);
	fprintf(stderr, "name %s size %d rank %d is_local %d is_service %d\n",
			set->name, set->size, set->self, set->is_local,
			set->is_service);

	mcl_finalize(proc_state);
	NA_Finalize(na_class);
	return 0;
}
