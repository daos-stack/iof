#include <stdbool.h>
#include <string.h>
#include <libgen.h>
#include <CUnit/Basic.h>
#include <iof_preload.h>
#include "intercept.h"

static bool preload_enabled;
const char ioil_name[] = "libioil.so";

int init_suite(void)
{
	char *preload_env = getenv("LD_PRELOAD");
	char *value;
	char *saveptr;
	char *token;
	char *libname;

	if (preload_env == NULL)
		goto done;

	value = strdup(preload_env);
	if (value == NULL)
		return CUE_NOMEMORY;

	token = strtok_r(value, ":", &saveptr);
	while (token != NULL) {
		libname = basename(token);
		if (strncmp(libname, ioil_name, sizeof(ioil_name)) == 0) {
			preload_enabled = true;
			break;
		}

		token = strtok_r(NULL, ":", &saveptr);
	}

	free(value);
done:
	printf("LD_PRELOAD is %s\n", preload_enabled ? "enabled" : "disabled");

	return CUE_SUCCESS;
}

int clean_suite(void)
{
	return CUE_SUCCESS;
}

#define CHECK_API(type, name, params)                       \
	do {                                                \
		if (preload_enabled) {                      \
			CU_ASSERT_PTR_NOT_NULL(iof_##name); \
		} else {                                    \
			CU_ASSERT_PTR_NULL(iof_##name);     \
		}                                           \
	} while (0);

static void test_iof_preload(void)
{
	CHECK_API(nop1, get_bypass_status, nop2)
	FOREACH_INTERCEPT(CHECK_API)
}

int main(int argc, char **argv)
{
	CU_pSuite pSuite = NULL;

	if (CU_initialize_registry() != CUE_SUCCESS)
		return CU_get_error();
	pSuite = CU_add_suite("iof_preload.h tests", init_suite,
			      clean_suite);
	if (!pSuite) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (!CU_add_test(pSuite, "Test iof_preload.h", test_iof_preload)) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	CU_cleanup_registry();

	return CU_get_error();

}
