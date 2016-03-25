#include <stdio.h>
#include "include/proto_common.h"

/**
 * this function is a copy paste from the Mercury tester.
 */
static na_return_t
my_na_addr_lookup_cb(const struct na_cb_info *callback_info)
{
	na_addr_t *addr_ptr = (na_addr_t *) callback_info->arg;
	na_return_t ret = NA_SUCCESS;

	if (callback_info->ret != NA_SUCCESS) {
		fprintf(stderr, "Return from callback with %s error code\n",
				NA_Error_to_string(callback_info->ret));
		return ret;
	}

	*addr_ptr = callback_info->info.lookup.addr;

	return ret;
}

/**
 * this function is a copy paste from the Mercury tester.
 */
na_return_t
my_na_addr_lookup_wait(na_class_t *na_class, const char *name, na_addr_t *addr)
{
	na_addr_t new_addr = NULL;
	na_bool_t lookup_completed = NA_FALSE;
	na_context_t *context = NULL;
	na_return_t ret = NA_SUCCESS;

	context = NA_Context_create(na_class);
	if (!context) {
		fprintf(stderr, "Could not create context\n");
		goto done;
	}

	ret = NA_Addr_lookup(na_class, context, &my_na_addr_lookup_cb,
			     &new_addr, name, NA_OP_ID_IGNORE);
	if (ret != NA_SUCCESS) {
		fprintf(stderr, "Could not start NA_Addr_lookup\n");
		goto done;
	}

	while (!lookup_completed) {
		na_return_t trigger_ret;
		unsigned int actual_count = 0;

		do {
			trigger_ret = NA_Trigger(context, 0, 1, &actual_count);
		} while ((trigger_ret == NA_SUCCESS) && actual_count);

		if (new_addr) {
			lookup_completed = NA_TRUE;
			*addr = new_addr;
		}

		if (lookup_completed)
			break;

		ret = NA_Progress(na_class, context, NA_MAX_IDLE_TIME);
		if (ret != NA_SUCCESS) {
			fprintf(stderr, "Could not make progress\n");
			goto done;
		}
	}

	ret = NA_Context_destroy(na_class, context);
	if (ret != NA_SUCCESS) {
		fprintf(stderr, "Could not destroy context\n");
		goto done;
	}

done:
	return ret;
}
