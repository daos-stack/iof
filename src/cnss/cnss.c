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
#include <inttypes.h>
#include <errno.h>
#include <dlfcn.h>
#ifdef IOF_USE_FUSE3
#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>
#else
#include <fuse/fuse.h>
#include <fuse/fuse_lowlevel.h>
#endif
#include <sys/xattr.h>
#include <sys/queue.h>
#include <crt_api.h>
#include <crt_util/common.h>

#include "cnss_plugin.h"
#include "version.h"
#include "log.h"
#include "iof.h"
#include "ctrl_common.h"

struct plugin_entry {
	struct cnss_plugin *plugin;
	void *dl_handle;
	size_t size;
	LIST_ENTRY(plugin_entry) list;
	int active;
};

LIST_HEAD(cnss_plugin_list, plugin_entry);

LIST_HEAD(fs_list, fs_info);

struct cnss_info {
	struct fs_list fs_head;
};

struct fs_info {
	char *mnt;
	struct fuse *fuse;
#if !IOF_USE_FUSE3
	struct fuse_chan *ch;
#endif
	pthread_t thread;
	struct fs_handle *fs_handle;

	LIST_ENTRY(fs_info) entries;
};


#define FN_TO_PVOID(fn) (*((void **)&(fn)))

/*
 * Helper macro only, do not use other then in CALL_PLUGIN_*
 *
 * Unfortunately because of this macros use of continue it does not work
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
 * Call a function in each registered and active plugin.  If the plugin
 * return non-zero disable the plugin.
 */
#define CALL_PLUGIN_FN_CHECK(LIST, FN)					\
	do {								\
		struct plugin_entry *_li;				\
		IOF_LOG_INFO("Calling plugin %s", #FN);			\
		LIST_FOREACH(_li, LIST, list) {				\
			int _rc;					\
			CHECK_PLUGIN_FUNCTION(_li, FN);			\
			_rc = _li->plugin->FN(_li->plugin->handle);	\
			if (_rc != 0) {					\
				IOF_LOG_INFO("Disabling plugin %s %d",	\
					_li->plugin->name, _rc);	\
				_li->active = 0;			\
			}						\
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

/*
 * Call a function in each registered and active plugin, providing additional
 * parameters.  If the function returns non-zero then disable the plugin
 */
#define CALL_PLUGIN_FN_START(LIST, FN, ...)				\
	do {								\
		struct plugin_entry *_li;				\
		int _rc;						\
		IOF_LOG_INFO("Calling plugin %s", #FN);			\
		LIST_FOREACH(_li, LIST, list) {				\
			CHECK_PLUGIN_FUNCTION(_li, FN);			\
			_rc = _li->plugin->FN(_li->plugin->handle,	\
					      __VA_ARGS__);		\
			if (_rc != 0) {					\
				IOF_LOG_INFO("Disabling plugin %s %d",	\
					     _li->plugin->name, _rc);	\
				_li->active = 0;			\
			}						\
		}							\
		IOF_LOG_INFO("Finished calling plugin %s", #FN);	\
	} while (0)

/* Load a plugin from a fn pointer, return -1 if there was a fatal problem */
static int add_plugin(struct cnss_plugin_list *plugin_list,
		      cnss_plugin_init_t fn, void *dl_handle)
{
	struct plugin_entry *entry;
	int rc;

	IOF_LOG_INFO("Loading plugin at entry point %p", FN_TO_PVOID(fn));

	entry = calloc(1, sizeof(struct plugin_entry));
	if (!entry)
		return CNSS_ERR_NOMEM;

	rc = fn(&entry->plugin, &entry->size);
	if (rc != 0) {
		free(entry);
		IOF_LOG_INFO("Plugin at entry point %p failed (%d)",
			     FN_TO_PVOID(fn), rc);
		return CNSS_SUCCESS;
	}

	entry->dl_handle = dl_handle;

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

	return CNSS_SUCCESS;
}

static void iof_fuse_umount(struct fs_info *info)
{
#if IOF_USE_FUSE3
	fuse_unmount(info->fuse);
#else
	fuse_unmount(info->mnt, info->ch);
#endif
}

/*
 * Creates a fuse filesystem for any plugin that needs one.
 *
 * Should be called from the post_start plugin callback and creates
 * a filesystem.
 * Returns 0 on success, or non-zero on error.
 */
static int register_fuse(void *handle, struct fuse_operations *ops,
			 const char *mnt, void *private_data)
{
	struct fs_info *info = NULL;
	struct cnss_info *cnss_info = (struct cnss_info *)handle;
	struct fuse_args args = {0};
	char *dash_d = "-d";

	if (!mnt) {
		IOF_LOG_ERROR("Invalid Mount point");
		return 1;
	}

	args.argc = 1;
	args.argv = &dash_d;
	args.allocated = 0;

	info = calloc(1, sizeof(struct fs_info));
	if (!info) {
		IOF_LOG_ERROR("Could not allocate fuse info");
		goto cleanup;
	}

	info->mnt = strdup(mnt);
	if (!info->mnt) {
		IOF_LOG_ERROR("Could not allocate mnt");
		goto cleanup;
	}
	if ((mkdir(info->mnt, 0755) && errno != EEXIST)) {
		IOF_LOG_ERROR("Could not create directory %s for mounting",
				info->mnt);
		goto cleanup;
	}

#if !IOF_USE_FUSE3
	info->ch = fuse_mount(info->mnt, &args);
	if (!info->ch) {
		IOF_LOG_ERROR("Could not successfully mount %s", info->mnt);
		goto cleanup;
	}
#endif

	info->fs_handle = private_data;
#if IOF_USE_FUSE3
	info->fuse = fuse_new(&args, ops,
			sizeof(struct fuse_operations), private_data);
#else
	info->fuse = fuse_new(info->ch, &args, ops,
			sizeof(struct fuse_operations), private_data);
#endif

	if (!info->fuse) {
		IOF_LOG_ERROR("Could not initialize fuse");
		fuse_opt_free_args(&args);
		iof_fuse_umount(info);
		goto cleanup;
	}

#if IOF_USE_FUSE3
	{
		int rc;

		rc = fuse_mount(info->fuse, info->mnt);
		if (rc != 0) {
			IOF_LOG_ERROR("Could not successfully mount %s",
				      info->mnt);
			goto cleanup;
		}
	}
#endif

	IOF_LOG_DEBUG("Registered a fuse mount point at : %s", info->mnt);

	fuse_opt_free_args(&args);

	LIST_INSERT_HEAD(&cnss_info->fs_head, info, entries);

	return 0;
cleanup:
	if (info->mnt)
		free(info->mnt);
	if (private_data)
		free(private_data);
	if (info)
		free(info);

	return 1;
}

static int deregister_fuse(struct fs_info *info)
{
	char *val = "1";

	fuse_session_exit(fuse_get_session(info->fuse));
#ifdef __APPLE__
	setxattr(info->mnt, "user.exit", val, strlen(val), 0, 0);
#else
	setxattr(info->mnt, "user.exit", val, strlen(val), 0);
#endif
	IOF_LOG_DEBUG("Unmounting FS: %s", info->mnt);
	pthread_join(info->thread, 0);
	LIST_REMOVE(info, entries);

	free(info->mnt);
	free(info->fs_handle);
	free(info);
	return CNSS_SUCCESS;
}

static void *loop_fn(void *args)
{
	int ret;
	struct fs_info *info = (struct fs_info *)args;

	/*Blocking*/
	ret = fuse_loop(info->fuse);

	if (ret != 0)
		IOF_LOG_DEBUG("Fuse loop exited with return code: %d", ret);

	iof_fuse_umount(info);
	fuse_destroy(info->fuse);
	return NULL;
}

int launch_fs(struct cnss_info *cnss_info)
{
	int ret;
	struct fs_info *info;

	LIST_FOREACH(info, &cnss_info->fs_head, entries) {
		IOF_LOG_INFO("Starting a FUSE filesystem at %s", info->mnt);
		ret = pthread_create(&info->thread, NULL, loop_fn,
				info);
		if (ret) {
			IOF_LOG_ERROR("Could not start FUSE filesysten at %s",
					info->mnt);
			iof_fuse_umount(info);
			fuse_destroy(info->fuse);
			return CNSS_ERR_PTHREAD;
		}
	}
	return ret;
}

void shutdown_fs(struct cnss_info *cnss_info)
{
	int ret;
	struct fs_info *info;

	ret = CNSS_SUCCESS;
	while (!LIST_EMPTY(&cnss_info->fs_head)) {
		info = LIST_FIRST(&cnss_info->fs_head);
		ret = deregister_fuse(info);
		if (ret)
			IOF_LOG_ERROR("Shutdown mount %s failed", info->mnt);
	}
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
	.register_fuse_fs = register_fuse,
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
	char *cnss = "CNSS";
	char *plugin_file = NULL;
	const char *prefix;
	char *version = iof_get_version();
	struct plugin_entry *list_iter;
	struct cnss_plugin_list plugin_list;
	struct cnss_info *cnss_info;
	int active_plugins = 0;

	int ret;
	int service_process_set = 0;
	char *ctrl_prefix;

	iof_log_init("CN", "CNSS");

	IOF_LOG_INFO("CNSS version: %s", version);

	prefix = getenv("CNSS_PREFIX");

	if (prefix == NULL) {
		IOF_LOG_ERROR("CNSS_PREFIX is required");
		return CNSS_ERR_PREFIX;
	}

	ret = asprintf(&ctrl_prefix, "%s/.ctrl", prefix);

	if (ret == -1) {
		IOF_LOG_ERROR("Could not allocate memory for ctrl prefix");
		return CNSS_ERR_NOMEM;
	}

	ret = ctrl_fs_start(ctrl_prefix);
	if (ret != 0) {
		IOF_LOG_ERROR("Could not start ctrl fs");
		return CNSS_ERR_CTRL_FS;
	}
	free(ctrl_prefix);

	LIST_INIT(&plugin_list);

	/* Load the build-in iof "plugin" */
	ret = add_plugin(&plugin_list, iof_plugin_init, NULL);
	if (ret != 0)
		return CNSS_ERR_PLUGIN;

	/* Check to see if an additional plugin file has been requested and
	 * attempt to load it
	 */
	plugin_file = getenv("CNSS_PLUGIN_FILE");
	if (plugin_file) {
		void *dl_handle = dlopen(plugin_file, RTLD_LAZY);
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
			ret = add_plugin(&plugin_list, fn, dl_handle);
			if (ret != 0)
				return CNSS_ERR_PLUGIN;
		}
	}

	/* Walk the list of plugins and if any require the use of a service
	 * process set across the CNSS nodes then create one
	 */
	LIST_FOREACH(list_iter, &plugin_list, list) {
		if (list_iter->active && list_iter->plugin->require_service) {
			service_process_set = CRT_FLAG_BIT_SERVER;
			break;
		}
	}

	IOF_LOG_INFO("Forming %s process set",
		     service_process_set ? "service" : "client");

	cnss_info = calloc(1, sizeof(struct cnss_info));
	if (!cnss_info)
		return CNSS_ERR_NOMEM;

	LIST_INIT(&cnss_info->fs_head);
	cnss_plugin_cb.handle = cnss_info;
	/*initialize CaRT*/
	ret = crt_init(cnss, service_process_set);
	if (ret) {
		IOF_LOG_ERROR("crt_init failed with ret = %d", ret);
		return CNSS_ERR_CART;
	}

	/* Call start for each plugin which should perform none-local
	 * operations only.  Plugins can choose to disable themselves
	 * at this point.
	 */
	CALL_PLUGIN_FN_START(&plugin_list, start, &cnss_plugin_cb,
			     sizeof(cnss_plugin_cb));

	/* Call post_start for each plugin, which could communicate over
	 * the network.  Plugins can choose to disable themselves
	 * at this point.
	 */
	CALL_PLUGIN_FN_CHECK(&plugin_list, post_start);

	/* Walk the plugins and check for active ones */
	LIST_FOREACH(list_iter, &plugin_list, list) {
		if (list_iter->active) {
			active_plugins = 1;
			break;
		}
	}

	/* TODO: How to handle this case? */
	if (!active_plugins) {
		IOF_LOG_ERROR("No active plugins");
		ctrl_fs_stop();
		return 1;
	}

	launch_fs(cnss_info);
	register_cnss_controls(1, &plugin_list);
	ctrl_fs_wait(); /* Blocks until ctrl_fs is shutdown */

	CALL_PLUGIN_FN(&plugin_list, flush);

	/* TODO: This doesn't seem right.   After flush, plugins can still
	 * actively send RPCs.   We really need a barrier here.  Then
	 * call finish.   Then finalize.
	 */
	shutdown_fs(cnss_info);
	CALL_PLUGIN_FN(&plugin_list, finish);

	ret = crt_finalize();
	while (!LIST_EMPTY(&plugin_list)) {
		struct plugin_entry *entry = LIST_FIRST(&plugin_list);

		LIST_REMOVE(entry, list);
		if (entry->dl_handle != NULL)
			dlclose(entry->dl_handle);
		free(entry);
	}

	iof_log_close();
	free(cnss_info);
	return ret;
}
