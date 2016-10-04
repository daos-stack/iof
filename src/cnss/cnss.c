/* Copyright (C) 2016 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted for any purpose (including commercial purposes)
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the
 *    documentation and/or materials provided with the distribution.
 *
 * 3. In addition, redistributions of modified forms of the source or binary
 *    code must carry prominent notices stating that the original code was
 *    changed and the date of the change.
 *
 *  4. All publications or advertising materials mentioning features or use of
 *     this software are asked, but not required, to acknowledge that it was
 *     developed by Intel Corporation and credit the contributors.
 *
 * 5. Neither the name of Intel Corporation, nor the name of any Contributor
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <string.h>
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
#include "ctrl_common.h"

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

static const char *get_config_option(const char *var)
{
	return getenv((const char *)var);
}

static struct cnss_plugin_cb cnss_plugin_cb = {
#ifdef IOF_USE_FUSE3
	.fuse_version = 3,
#else /* ! IOF_USE_FUSE3 */
	.fuse_version = 2,
#endif /* IOF_USE_FUSE3 */
	.get_config_option = get_config_option,
	.register_ctrl_variable = ctrl_register_variable,
	.register_ctrl_event = ctrl_register_event,
	.register_ctrl_counter = ctrl_register_counter,
	.register_ctrl_constant = ctrl_register_constant,
};

int cnss_shutdown(void *arg)
{
	/* TODO: broadcast the shutdown to other CNSS nodes */

	return 0;
}

int cnss_client_attach(int client_id, void *arg)
{
	struct cnss_plugin_list *plugin_list;

	plugin_list = (struct cnss_plugin_list *)arg;

	if (plugin_list == NULL)
		return 0;

	CALL_PLUGIN_FN_PARAM(plugin_list, client_attached, client_id);

	return 0;
}

int cnss_client_detach(int client_id, void *arg)
{
	struct cnss_plugin_list *plugin_list;

	plugin_list = (struct cnss_plugin_list *)arg;

	if (plugin_list == NULL)
		return 0;

	CALL_PLUGIN_FN_PARAM(plugin_list, client_detached, client_id);

	return 0;
}

int main(void)
{
	struct mcl_set *cnss_set;
	struct mcl_state *state;
	struct mcl_set *ionss_set;
	char *plugin_file = NULL;
	const char *prefix;
	char *version = iof_get_version();
	struct plugin_entry *list_iter;
	struct cnss_plugin_list plugin_list;
	int ret;
	int service_process_set = 0;
	char *ctrl_prefix;

	iof_log_init("cnss");

	IOF_LOG_INFO("CNSS version: %s", version);

	prefix = getenv("CNSS_PREFIX");

	if (prefix == NULL) {
		IOF_LOG_ERROR("CNSS_PREFIX is required");
		return -1;
	}

	ret = asprintf(&ctrl_prefix, "%s/.ctrl", prefix);

	if (ret == -1) {
		IOF_LOG_ERROR("Could not allocate memory for ctrl prefix");
		return -1;
	}

	ret = ctrl_fs_start(ctrl_prefix);
	if (ret != 0) {
		IOF_LOG_ERROR("Could not start ctrl fs");
		return -1;
	}
	free(ctrl_prefix);

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
			fn = (cnss_plugin_init_t)dlsym(dl_handle,
						       CNSS_PLUGIN_INIT_SYMBOL);

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

	CALL_PLUGIN_FN_PARAM(&plugin_list, start, state, &cnss_plugin_cb,
			     sizeof(cnss_plugin_cb));
	mcl_startup(state, "cnss", 1, &cnss_set);

	ret = mcl_attach(state, "ionss", &ionss_set);
	if (ret != MCL_SUCCESS) {
		IOF_LOG_ERROR("Attach to IONSS Failed");
		return 1;
	}

	CALL_PLUGIN_FN_PARAM(&plugin_list, post_start, ionss_set);

	register_cnss_controls(cnss_set->size, &plugin_list);
	ctrl_fs_wait(); /* Blocks until ctrl_fs is shutdown */

	CALL_PLUGIN_FN(&plugin_list, flush);

	/* TODO: This doesn't seem right.   After flush, plugins can still
	 * actively send RPCs.   We really need a barrier here.  Then
	 * call finish.   Then finalize.
	 */
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
