#include <stdio.h>
#include <mercury.h>
#include <inttypes.h>
#include <process_set.h>
#include <mercury.h>
#include <errno.h>
#include <dlfcn.h>

#include <process_set.h>
#include <mcl_event.h>

#include "iof_common.h"
#include "version.h"
#include "log.h"
#include "iof.h"

struct plugin_entry {
	struct cnss_plugin *plugin;
	size_t size;
	LIST_ENTRY(plugin_entry) list;
	int active;
};

LIST_HEAD(cnss_plugin_list, plugin_entry);

#define FN_TO_PVOID(fn) (*((void **)&(fn)))

/*
 * Helper macro only, do not use other then in CALL_PLUGIN_*
 *
 * Unfortunatly because of this macros use of continue it does not work
 * correctly if contained in a do - while loop.
 */
#define CHECK_PLUGIN_FUNCTION(ITER, FN)					\
	if (!ITER->active)						\
		continue;						\
	if (!ITER->plugin->FN)						\
		continue;						\
	if ((offsetof(struct cnss_plugin, FN) + sizeof(void *)) > ITER->size) \
		continue;						\
	IOF_LOG_INFO("Plugin %s(%p) calling %s at %p",			\
		ITER->plugin->name,					\
		(void *)ITER->plugin->handle,				\
		#FN,							\
		FN_TO_PVOID(ITER->plugin->FN))

/*
 * Call a function in each registered and active plugin
 */
#define CALL_PLUGIN_FN(LIST, FN)					\
	do {								\
		struct plugin_entry *_li;				\
		IOF_LOG_INFO("Calling plugin %s", #FN);			\
		LIST_FOREACH(_li, LIST, list) {				\
			CHECK_PLUGIN_FUNCTION(_li, FN);			\
			_li->plugin->FN(_li->plugin->handle);		\
		}							\
		IOF_LOG_INFO("Finished calling plugin %s", #FN);	\
	} while (0)

/*
 * Call a function in each registered and active plugin, providing additional
 * parameters.
 */
#define CALL_PLUGIN_FN_PARAM(LIST, FN, ...)				\
	do {								\
		struct plugin_entry *_li;				\
		IOF_LOG_INFO("Calling plugin %s", #FN);			\
		LIST_FOREACH(_li, LIST, list) {				\
			CHECK_PLUGIN_FUNCTION(_li, FN);			\
			_li->plugin->FN(_li->plugin->handle, __VA_ARGS__); \
		}							\
		IOF_LOG_INFO("Finished calling plugin %s", #FN);	\
	} while (0)

/* Load a plugin from a fn pointer, return -1 if there was a fatal problem */
static int add_plugin(struct cnss_plugin_list *plugin_list,
		      cnss_plugin_init_t fn)
{
	struct plugin_entry *entry;
	int rc;

	IOF_LOG_INFO("Loading plugin at entry point %p", FN_TO_PVOID(fn));

	entry = calloc(1, sizeof(struct plugin_entry));
	if (!entry)
		return -1;

	rc = fn(&entry->plugin, &entry->size);
	if (rc != 0) {
		free(entry);
		IOF_LOG_INFO("Plugin at entry point %p failed (%d)",
			     FN_TO_PVOID(fn), rc);
		return 0;
	}

	LIST_INSERT_HEAD(plugin_list, entry, list);

	IOF_LOG_INFO("Added plugin %s(%p) from entry point %p ",
		     entry->plugin->name,
		     (void *)entry->plugin->handle,
		     FN_TO_PVOID(fn));

	if (entry->plugin->version == CNSS_PLUGIN_VERSION) {
		entry->active = 1;
	} else {
		IOF_LOG_INFO("Plugin %s(%p) version incorrect %x %x, disabling",
			     entry->plugin->name,
			     (void *)entry->plugin->handle,
			     entry->plugin->version,
			     CNSS_PLUGIN_VERSION);
	}

	if (sizeof(struct cnss_plugin) != entry->size) {
		IOF_LOG_INFO("Plugin %s(%p) size incorrect %zd %zd, some functions may be disabled",
			     entry->plugin->name,
			     (void *)entry->plugin->handle,
			     entry->size,
			     sizeof(struct cnss_plugin));
	}

	return 0;
}

int main(void)
{
	struct mcl_set *cnss_set;
	struct mcl_state *state;
	struct mcl_set *ionss_set;
	int ret;
	struct plugin_entry *list_iter;
	struct cnss_plugin_list plugin_list;
	int service_process_set = 0;
	char *plugin_file = NULL;

	char *version = iof_get_version();
	iof_log_init("cnss");

	IOF_LOG_INFO("CNSS version: %s", version);

	LIST_INIT(&plugin_list);

	/* Load the build-in iof "plugin" */
	ret = add_plugin(&plugin_list, iof_plugin_init);
	if (ret != 0)
		return 1;

	/* Check to see if an additional plugin file has been requested and
	 * attempt to load it
	 */
	plugin_file = getenv("CNSS_PLUGIN_FILE");
	if (plugin_file) {
		void *dl_handle = dlopen(plugin_file, 0);
		cnss_plugin_init_t fn = NULL;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

		if (dl_handle)
			fn = (cnss_plugin_init_t)dlsym(dl_handle, CNSS_PLUGIN_INIT_SYMBOL);

#pragma GCC diagnostic pop

		IOF_LOG_INFO("Loading plugin file %s %p %p",
			     plugin_file,
			     dl_handle,
			     FN_TO_PVOID(fn));
		if (fn) {
			ret = add_plugin(&plugin_list, fn);
			if (ret != 0)
				return 1;
		}
	}

	state = mcl_init();
	if (!state) {
		IOF_LOG_ERROR("mcl_init() failed");
		return 1;
	}

	/* Walk the list of plugins and if any require the use of a service
	 * process set across the CNSS nodes then create one
	 */
	LIST_FOREACH(list_iter, &plugin_list, list) {
		if (list_iter->active && list_iter->plugin->require_service)
			service_process_set = 1;
	}

	IOF_LOG_INFO("Forming %s process set",
		     service_process_set ? "service" : "client");

	CALL_PLUGIN_FN_PARAM(&plugin_list, start, state, NULL, 0);
	mcl_startup(state, "cnss", 1, &cnss_set);

	ret = mcl_attach(state, "ionss", &ionss_set);
	if (ret != MCL_SUCCESS) {
		IOF_LOG_ERROR("Attach to IONSS Failed");
		return 1;
	}

	CALL_PLUGIN_FN_PARAM(&plugin_list, post_start, ionss_set);

	CALL_PLUGIN_FN(&plugin_list, flush);

	mcl_detach(state, ionss_set);
	mcl_finalize(state);
	CALL_PLUGIN_FN(&plugin_list, finish);

	while (!LIST_EMPTY(&plugin_list)) {
		struct plugin_entry *entry = LIST_FIRST(&plugin_list);

		LIST_REMOVE(entry, list);
		free(entry);
	}

	iof_log_close();
	return ret;
}
