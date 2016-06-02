#include <stdio.h>
#include <CUnit/Basic.h>

#include <ios_gah.h>

int init_suite(void)
{
	return CUE_SUCCESS;
}

int clean_suite(void)
{
	return CUE_SUCCESS;
}

/** test the size of the GAH struct is indeed 128 bits */
static void test_ios_gah_size(void)
{
	CU_ASSERT(sizeof(struct ios_gah)*8 == 128);
}

/** test ios_gah_init() */
static void test_ios_gah_init(void)
{
	struct ios_gah_store *ios_gah_store = NULL;

	ios_gah_store = ios_gah_init();
	CU_ASSERT(ios_gah_store != NULL);
	CU_ASSERT_FATAL(ios_gah_destroy(ios_gah_store) == IOS_SUCCESS);
}

/** test ios_gah_destroy() */
static void test_ios_gah_destroy(void)
{
	struct ios_gah_store *ios_gah_store = NULL;

	ios_gah_store = ios_gah_init();
	CU_ASSERT(ios_gah_store != NULL);
	if (ios_gah_store == NULL)
		return;
	CU_ASSERT(ios_gah_destroy(ios_gah_store) == IOS_SUCCESS);
	CU_ASSERT(ios_gah_destroy(NULL) == IOS_ERR_INVALID_PARAM);
}

/** test ios_gah_allocate() */
static void test_ios_gah_allocate(void)
{
	int rc = IOS_SUCCESS;
	struct ios_gah *ios_gah;
	struct ios_gah_store *ios_gah_store;
	int ii;
	int num_handles = 1024*20;
	void *data = NULL;

	CU_ASSERT(sizeof(struct ios_gah)*8 == 128);
	ios_gah_store = ios_gah_init();
	CU_ASSERT_FATAL(ios_gah_store != NULL);
	ios_gah = (struct ios_gah *) calloc(num_handles, sizeof(struct
					      ios_gah));
	CU_ASSERT_FATAL(ios_gah != NULL);

	for (ii = 0; ii < num_handles; ii++) {
		data = malloc(512);
		CU_ASSERT_FATAL(data != NULL);
		rc |= ios_gah_allocate(ios_gah_store, ios_gah + ii, 0, 0, data);
	}
	CU_ASSERT(rc == IOS_SUCCESS);
	rc = ios_gah_allocate(NULL, ios_gah, 0, 0, data);
	CU_ASSERT(rc == IOS_ERR_INVALID_PARAM);
	rc = ios_gah_allocate(ios_gah_store, NULL, 0, 0, data);
	CU_ASSERT(rc == IOS_ERR_INVALID_PARAM);

	for (ii = 0; ii < num_handles; ii++) {
		free(ios_gah_store->ptr_array[(ios_gah + ii)->fid]->internal);
		CU_ASSERT_FATAL(ios_gah_deallocate(ios_gah_store, ios_gah + ii)
				== IOS_SUCCESS);
	}

	ios_gah_destroy(ios_gah_store);
	free(ios_gah);
}

/** test utility routines  */
static void test_ios_gah_misc(void)
{
	int rc = IOS_SUCCESS;
	struct ios_gah *ios_gah;
	char *ios_str = NULL;
	struct ios_gah_store *ios_gah_store;
	int ii;
	int num_handles = 1024*20;
	void *data = NULL;
	void *internal = NULL;

	CU_ASSERT(sizeof(struct ios_gah)*8 == 128);
	ios_gah_store = ios_gah_init();
	CU_ASSERT_FATAL(ios_gah_store != NULL);
	ios_gah = (struct ios_gah *) calloc(num_handles, sizeof(struct
					      ios_gah));
	CU_ASSERT_FATAL(ios_gah != NULL);

	for (ii = 0; ii < num_handles; ii++) {
		data = malloc(512);
		CU_ASSERT_FATAL(data != NULL);
		rc |= ios_gah_allocate(ios_gah_store, ios_gah + ii, 0, 0, data);
	}
	CU_ASSERT(rc == IOS_SUCCESS);


	/** test ios_gah_check_crc() */
	rc = ios_gah_check_crc(NULL);
	CU_ASSERT(rc == IOS_ERR_INVALID_PARAM);
	rc = ios_gah_check_crc(ios_gah);
	CU_ASSERT(rc == IOS_SUCCESS);
	ios_gah->root++;
	rc = ios_gah_check_crc(ios_gah);
	CU_ASSERT(rc == IOS_ERR_CRC_MISMATCH);
	ios_gah->root--;

	/** test ios_gah_check_version() */
	rc = ios_gah_check_version(NULL);
	CU_ASSERT(rc == IOS_ERR_INVALID_PARAM);
	rc = ios_gah_check_version(ios_gah);
	CU_ASSERT(rc == IOS_SUCCESS);
	ios_gah->version++;
	rc = ios_gah_check_version(ios_gah);
	CU_ASSERT(rc == IOS_ERR_VERSION_MISMATCH);
	ios_gah->version--;

	/** test ios_gah_is_self_root() */
	rc = ios_gah_is_self_root(NULL, 0);
	CU_ASSERT(rc == IOS_ERR_INVALID_PARAM);
	rc = ios_gah_is_self_root(ios_gah, 0);
	CU_ASSERT(rc == IOS_SUCCESS);
	rc = ios_gah_is_self_root(ios_gah, 2);
	CU_ASSERT(rc == IOS_ERR_OTHER);

	/** test ios_gah_to_str() */
	ios_str = ios_gah_to_str(NULL);
	CU_ASSERT(ios_str == NULL);
	ios_str = ios_gah_to_str(ios_gah);
	CU_ASSERT(ios_str != NULL);
	if (ios_str)
		free(ios_str);

	/** test ios_gah_get_info() */
	CU_ASSERT(ios_gah_get_info(NULL, ios_gah, &internal) != IOS_SUCCESS);
	CU_ASSERT(ios_gah_get_info(ios_gah_store, NULL, &internal) !=
			IOS_SUCCESS);
	CU_ASSERT(ios_gah_get_info(ios_gah_store, ios_gah, NULL) !=
			IOS_SUCCESS);

	for (ii = 0; ii < num_handles; ii++) {
		free(ios_gah_store->ptr_array[(ios_gah + ii)->fid]->internal);
		CU_ASSERT_FATAL(ios_gah_deallocate(ios_gah_store, ios_gah + ii)
				== IOS_SUCCESS);
	}
	CU_ASSERT(ios_gah_get_info(ios_gah_store, ios_gah, &internal) !=
			IOS_SUCCESS);

	ios_gah_destroy(ios_gah_store);
	free(ios_gah);
}

int main(int argc, char **argv)
{
	CU_pSuite pSuite = NULL;

	if (CU_initialize_registry() != CUE_SUCCESS)
		return CU_get_error();
	pSuite = CU_add_suite("GAH API test", init_suite, clean_suite);
	if (!pSuite) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (!CU_add_test(pSuite, "sizeof(struct ios_gah) test",
		    test_ios_gah_size) ||
	    !CU_add_test(pSuite, "ios_gah_init() test", test_ios_gah_init) ||
	    !CU_add_test(pSuite, "ios_gah_allocate() test",
		    test_ios_gah_allocate) ||
	    !CU_add_test(pSuite, "ios_gah_destroy() test",
		    test_ios_gah_destroy) ||
	    !CU_add_test(pSuite, "ios_gah_misc test", test_ios_gah_misc)) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	CU_cleanup_registry();

	return CU_get_error();
}
