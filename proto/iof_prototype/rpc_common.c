
#include <assert.h>
#include<pthread.h>
#include <unistd.h>
#include "rpc_common.h"

static hg_context_t *hg_context;
static hg_class_t *hg_class;

static pthread_t hg_progress_tid;
static int hg_progress_shutdown_flag;

static void *progress_fn(void *foo);

hg_class_t *engine_init(int start_thread, struct mcl_state *state)
{
	int ret;

	hg_class = state->hg_class;
	hg_context = state->hg_context;

	if (start_thread) {
		ret = pthread_create(&hg_progress_tid, NULL, progress_fn, NULL);
		assert(ret == 0);
	}

	return hg_class;
}

void engine_finalize(void)
{
	int ret;

	/* tell progress thread to wrap things up */
	hg_progress_shutdown_flag = 1;

	/* wait for it to shutdown cleanly */
	ret = pthread_join(hg_progress_tid, NULL);
	assert(ret == 0);
}

static void *progress_fn(void *foo)
{
	hg_return_t ret;
	unsigned int actual_count;

	while (!hg_progress_shutdown_flag) {
		do {
			ret =
			    HG_Trigger(hg_context, 0, 1,
				       &actual_count);
		} while ((ret == HG_SUCCESS) && actual_count &&
			 !hg_progress_shutdown_flag);

		if (!hg_progress_shutdown_flag)
			HG_Progress(hg_context, 100);
	}

	return NULL;
}

hg_return_t engine_progress(struct mcl_event *done)
{
	unsigned int actual_count;
	hg_return_t ret;
	int rep_count = 0;

	do {
		ret = HG_Progress(hg_context, 1000);
		HG_Trigger(hg_context, 0, 1, &actual_count);
		if (mcl_event_test(done))
			return HG_SUCCESS;
	} while (rep_count++ < 5);
	return ret;
}

void engine_create_handle(na_addr_t addr, hg_id_t id, hg_handle_t *handle)
{
	hg_return_t ret;

	ret = HG_Create(hg_context, addr, id, handle);
	assert(ret == HG_SUCCESS);
}
