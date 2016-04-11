#include <assert.h>
#include <pthread.h>
#include <unistd.h>

#include "test_ps_common.h"

static pthread_t hg_progress_tid;
static int hg_progress_shutdown_flag;
static void *hg_progress_fn(void *foo);

void test_create_progress_thread(na_class_t *na_class)
{
	int ret;

	ret = pthread_create(&hg_progress_tid, NULL, hg_progress_fn,
			(void *) na_class);
	assert(ret == 0);
}

void test_destroy_progress_thread(void)
{
	int ret;

	hg_progress_shutdown_flag = 1;
	ret = pthread_join(hg_progress_tid, NULL);
	assert(ret == 0);
}

static void *hg_progress_fn(void *foo)
{
	na_return_t na_ret;
	unsigned int actual_count = 0;
	na_class_t *na_class;
	na_context_t *na_context;

	na_class = (na_class_t *) foo;
	na_context = NA_Context_create(na_class);
	assert(na_context);

	while (hg_progress_shutdown_flag == 0) {
		do {
			na_ret = NA_Trigger(na_context, 0, 1, &actual_count);
		} while (na_ret == NA_SUCCESS && actual_count);
		na_ret = NA_Progress(na_class, na_context, 500);
	}
	NA_Context_destroy(na_class, na_context);

	pthread_exit(NULL);
}
