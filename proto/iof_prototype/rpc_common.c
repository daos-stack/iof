
#include <assert.h>
#include<pthread.h>
#include <unistd.h>
#include "rpc_common.h"

static hg_context_t *hg_context;

static pthread_t hg_progress_tid;
static int hg_progress_shutdown_flag;

static void *progress_fn(void *foo);

hg_class_t *engine_init(int start_thread, struct mcl_state *state)
{
	int ret;

	hg_context = state->mcl_context->context;

	if (start_thread) {
		ret = pthread_create(&hg_progress_tid, NULL, progress_fn, NULL);
		assert(ret == 0);
	}

	return state->hg_class;
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

/* Block waiting for event to fire, or a timeout to occour.
 *
 * A timeout is six or more seconds with no network activity.
 */
hg_return_t engine_progress(struct mcl_event *done)
{
	int timeout_count = 0;

	do {
		unsigned int actual_count;
		hg_return_t ret;

		/* First drain the completion queue to process all events,
		 * checking for the done event along the way
		 */
		do {
			actual_count = 0;
			HG_Trigger(hg_context, 0, 5, &actual_count);
			if (mcl_event_test(done))
				return HG_SUCCESS;
		} while (actual_count > 0);

		/* Now block on the network waiting for activity */
		ret = HG_Progress(hg_context, 2000);

		/* If there was a timeout then record it, if there was activity
		 * then clear the timeout, if there is anything else then return
		 */
		if (ret == HG_TIMEOUT)
			timeout_count++;
		else if (ret == HG_SUCCESS)
			timeout_count = 0;
		else
			return ret;
	} while (timeout_count < 3);
	return HG_TIMEOUT;
}

void engine_create_handle(hg_addr_t addr, hg_id_t id, hg_handle_t *handle)
{
	hg_return_t ret;

	ret = HG_Create(hg_context, addr, id, handle);
	assert(ret == HG_SUCCESS);
}
