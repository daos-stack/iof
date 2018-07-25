/* Copyright (C) 2016-2018 Intel Corporation
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

#include <errno.h>
#include <getopt.h>
#include <dlfcn.h>
#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>

#include <cart/api.h>
#include <gurt/common.h>
#include <signal.h>

#include "cnss_plugin.h"
#include "version.h"
#include "log.h"
#include "ctrl_common.h"

#include "cnss.h"

/* A descriptor for the plugin */
struct plugin_entry {
	/* The callback functions, as provided by the plugin */
	struct cnss_plugin	*fns;
	/* The size of the fns struct */
	size_t			fns_size;

	/* The dl_open() reference to this so it can be closed cleanly */
	void			*dl_handle;

	/* The list of plugins */
	d_list_t		list;

	/* Flag to say if plugin is active */
	bool			active;

	/* The copy of the plugin->cnss callback functions this plugin uses */
	struct cnss_plugin_cb	self_fns;

	d_list_t		fuse_list;
};

struct fs_info {
	char		*mnt;
	struct fuse	*fuse;
	struct fuse_session *session;
	pthread_t	thread;
	pthread_mutex_t	lock;
	void		*private_data;
	d_list_t	entries;
	bool		running;
	bool		mt;
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
	if (!ITER->fns->FN)						\
		continue;						\
	if ((offsetof(struct cnss_plugin, FN) + sizeof(void *)) > ITER->fns_size) \
		continue;						\
	IOF_LOG_INFO("Plugin %s(%p) calling %s at %p",			\
		ITER->fns->name,					\
		(void *)ITER->fns->handle,				\
		#FN,							\
		FN_TO_PVOID(ITER->fns->FN))

/*
 * Call a function in each registered and active plugin
 */
#define CALL_PLUGIN_FN(LIST, FN)					\
	do {								\
		struct plugin_entry *_li;				\
		IOF_LOG_INFO("Calling plugin %s", #FN);			\
		d_list_for_each_entry(_li, LIST, list) {		\
			CHECK_PLUGIN_FUNCTION(_li, FN);			\
			_li->fns->FN(_li->fns->handle);		\
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
		d_list_for_each_entry(_li, LIST, list) {		\
			int _rc;					\
			CHECK_PLUGIN_FUNCTION(_li, FN);			\
			_rc = _li->fns->FN(_li->fns->handle);	\
			if (_rc != 0) {					\
				IOF_LOG_INFO("Disabling plugin %s %d",	\
					_li->fns->name, _rc);	\
				_li->active = false;			\
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
		d_list_for_each_entry(_li, LIST, list) {		\
			CHECK_PLUGIN_FUNCTION(_li, FN);			\
			_li->fns->FN(_li->fns->handle, __VA_ARGS__);	\
		}							\
		IOF_LOG_INFO("Finished calling plugin %s", #FN);	\
	} while (0)

/*
 * Call a function in each registered and active plugin, providing additional
 * parameters.  If the function returns non-zero then disable the plugin
 */
#define CALL_PLUGIN_FN_START(LIST, FN)					\
	do {								\
		struct plugin_entry *_li;				\
		int _rc;						\
		IOF_LOG_INFO("Calling plugin %s", #FN);			\
		d_list_for_each_entry(_li, LIST, list) {		\
			CHECK_PLUGIN_FUNCTION(_li, FN);			\
			_rc = _li->fns->FN(_li->fns->handle,		\
					   &_li->self_fns,		\
					   sizeof(struct cnss_plugin_cb));\
			if (_rc != 0) {					\
				IOF_LOG_INFO("Disabling plugin %s %d",	\
					     _li->fns->name, _rc);	\
				_li->active = false;			\
			}						\
		}							\
		IOF_LOG_INFO("Finished calling plugin %s", #FN);	\
	} while (0)

static const char *get_config_option(const char *var)
{
	return getenv((const char *)var);
}

static void iof_fuse_umount(struct fs_info *info)
{
	if (info->session)
		fuse_session_unmount(info->session);
	else
		fuse_unmount(info->fuse);
}

/* Add a NO-OP signal handler.  This doesn't do anything other than
 * interrupt the fuse leader thread if it's not already awake to
 * reap the other fuse threads.  Initially this was tried with a
 * no-op function however that didn't appear to work so perhaps the
 * compiler was optimising it away.
 */
static int signal_word;

static void iof_signal_poke(int signal)
{
	signal_word++;
}

static void *ll_loop_fn(void *args)
{
	int ret;
	struct fs_info *info = args;
	const struct sigaction act = {.sa_handler = iof_signal_poke};

	D_MUTEX_LOCK(&info->lock);
	info->running = true;
	D_MUTEX_UNLOCK(&info->lock);

	sigaction(SIGUSR1, &act, NULL);

	/*Blocking*/
	if (info->mt) {
		struct fuse_loop_config config = {.max_idle_threads = 10};

		ret = fuse_session_loop_mt(info->session, &config);
	} else {
		ret = fuse_session_loop(info->session);
	}
	if (ret != 0)
		IOF_LOG_ERROR("Fuse loop exited with return code: %d", ret);

	IOF_TRACE_DEBUG(info, "fuse loop completed %d", ret);

	D_MUTEX_LOCK(&info->lock);
	info->running = false;
	D_MUTEX_UNLOCK(&info->lock);
	return (void *)(uintptr_t)ret;
}

static void *loop_fn(void *args)
{
	int ret;
	struct fs_info *info = (struct fs_info *)args;

	D_MUTEX_LOCK(&info->lock);
	info->running = true;
	D_MUTEX_UNLOCK(&info->lock);

	/*Blocking*/
	if (info->mt) {
		struct fuse_loop_config config = {.max_idle_threads = 10};

		ret = fuse_loop_mt(info->fuse, &config);
	} else {
		ret = fuse_loop(info->fuse);
	}

	if (ret != 0)
		IOF_LOG_ERROR("Fuse loop exited with return code: %d", ret);

	D_MUTEX_LOCK(&info->lock);
	fuse_destroy(info->fuse);
	info->fuse = NULL;
	info->running = false;
	D_MUTEX_UNLOCK(&info->lock);

	return (void *)(uintptr_t)ret;
}

/*
 * Creates a fuse filesystem for any plugin that needs one.
 *
 * Should be called from the post_start plugin callback and creates
 * a filesystem.
 * Returns 0 on success, or non-zero on error.
 */
static bool
register_fuse(void *arg,
	      struct fuse_operations *ops,
	      struct fuse_lowlevel_ops *flo,
	      struct fuse_args *args,
	      const char *mnt,
	      bool threaded,
	      void *private_data,
	      struct fuse_session **sessionp)
{
	struct plugin_entry	*plugin = arg;
	struct fs_info		*info;
	int rc;

	if (!mnt) {
		IOF_TRACE_ERROR(plugin, "Invalid Mount point");
		return false;
	}

	errno = 0;
	rc = mkdir(mnt, 0755);
	if (rc != 0 && errno != EEXIST) {
		IOF_TRACE_ERROR(plugin,
				"Could not create directory %s for import",
				mnt);
		return false;
	}

	D_ALLOC_PTR(info);
	if (!info)
		return false;

	info->mt = threaded;

	/* TODO: The plugin should provide the sub-directory only, not the
	 * entire mount point and this function should add the cnss_prefix
	 */
	info->mnt = strdup(mnt);
	if (!info->mnt)
		goto cleanup_no_mutex;

	rc = D_MUTEX_INIT(&info->lock, NULL);
	if (rc != -DER_SUCCESS) {
		IOF_TRACE_ERROR(plugin, "Count not create mutex");
		goto cleanup_no_mutex;
	}

	info->private_data = private_data;

	if (flo) {
		/* TODO: Cleanup properly here */
		info->session = fuse_session_new(args,
						 flo,
						 sizeof(*flo),
						 private_data);
		if (!info->session)
			goto cleanup;

		rc = fuse_session_mount(info->session, info->mnt);
		if (rc != 0)
			goto cleanup;

		*sessionp = info->session;
	} else {
		info->fuse = fuse_new(args, ops, sizeof(*ops), private_data);

		if (!info->fuse) {
			IOF_TRACE_ERROR(plugin, "Could not initialize fuse");
			fuse_opt_free_args(args);
			iof_fuse_umount(info);
			goto cleanup;
		}

		rc = fuse_mount(info->fuse, info->mnt);
		if (rc != 0)
			goto cleanup;
	}

	IOF_TRACE_DEBUG(plugin,
			"Registered a fuse mount point at : %s", info->mnt);
	IOF_TRACE_DEBUG(plugin,
			"Private data %p threaded %u", private_data, info->mt);

	fuse_opt_free_args(args);

	if (flo)
		rc = pthread_create(&info->thread, NULL,
				    ll_loop_fn, info);
	else
		rc = pthread_create(&info->thread, NULL,
				    loop_fn, info);

	if (rc) {
		IOF_TRACE_ERROR(plugin,
				"Could not start FUSE filesysten at '%s'",
				info->mnt);
		iof_fuse_umount(info);
		goto cleanup;
	}

	d_list_add(&info->entries, &plugin->fuse_list);

	return true;
cleanup:
	rc = pthread_mutex_destroy(&info->lock);
	if (rc != 0)
		IOF_TRACE_ERROR(plugin,
				"Failed to destroy lock %d %s",
				rc, strerror(rc));
cleanup_no_mutex:
	D_FREE(info->mnt);
	D_FREE(info);

	return false;
}

static int
deregister_fuse(struct plugin_entry *plugin, struct fs_info *info)
{
	struct timespec wait_time;
	void *rcp = NULL;
	int rc;

	clock_gettime(CLOCK_REALTIME, &wait_time);

	D_MUTEX_LOCK(&info->lock);

	IOF_TRACE_DEBUG(info, "Unmounting FS: %s", info->mnt);

#if 0
	/* This will have already been called once */
	if (plugin->active && plugin->fns->flush_fuse)
		plugin->fns->flush_fuse(info->private_data);
#endif

	if (info->running) {
		IOF_TRACE_DEBUG(info,
				"Sending termination signal %s", info->mnt);

		/*
		 * If the FUSE thread is in the filesystem servicing requests
		 * then set the exit flag and send it a dummy operation to wake
		 * it up.  Drop the mutext before calling setxattr() as that
		 * will cause I/O activity and loop_fn() to deadlock with this
		 * function.
		 */
		if (info->session) {
			fuse_session_exit(info->session);
			fuse_session_unmount(info->session);
		} else {
			struct fuse_session *session = fuse_get_session(info->fuse);

			fuse_session_exit(session);
			fuse_session_unmount(session);
		}
	}

	D_MUTEX_UNLOCK(&info->lock);

	do {
		IOF_TRACE_INFO(info, "Trying to join fuse thread");

		wait_time.tv_sec++;

		rc = pthread_timedjoin_np(info->thread, &rcp, &wait_time);

		IOF_TRACE_INFO(info, "Join returned %d:'%s'", rc, strerror(rc));

		if (rc == ETIMEDOUT) {
			if (info->session &&
			    !fuse_session_exited(info->session))
				IOF_TRACE_WARNING(info,
						  "Session still running");

			IOF_TRACE_INFO(info,
				       "Thread still running, waking it up");

			pthread_kill(info->thread, SIGUSR1);
		}

	} while (rc == ETIMEDOUT);

	if (rc)
		IOF_TRACE_ERROR(info, "Final join returned %d:'%s'",
				rc, strerror(rc));

	d_list_del(&info->entries);

	rc = pthread_mutex_destroy(&info->lock);
	if (rc != 0)
		IOF_TRACE_ERROR(info,
				"Failed to destroy lock %d:'%s'",
				rc, strerror(rc));

	rc = (uintptr_t)rcp;

	if (plugin->active && plugin->fns->deregister_fuse) {
		int rcf = plugin->fns->deregister_fuse(info->private_data);

		if (rcf)
			rc = rcf;
	}

	if (info->session)
		fuse_session_destroy(info->session);

	return rc;
}

void flush_fs(struct cnss_info *cnss_info)
{
	struct plugin_entry *plugin;
	struct fs_info *info;

	d_list_for_each_entry(plugin, &cnss_info->plugins, list) {
		if (!plugin->active || !plugin->fns->flush_fuse)
			continue;

		d_list_for_each_entry(info, &plugin->fuse_list, entries) {
			if (!info->session)
				continue;

			plugin->fns->flush_fuse(info->private_data);
		}
	}
}

bool shutdown_fs(struct cnss_info *cnss_info)
{
	struct plugin_entry *plugin;
	struct fs_info *info, *i2;
	bool ok = true;
	int rc;

	d_list_for_each_entry(plugin, &cnss_info->plugins, list) {
		d_list_for_each_entry_safe(info, i2, &plugin->fuse_list,
					   entries) {
			rc = deregister_fuse(plugin, info);
			if (rc) {
				IOF_TRACE_ERROR(cnss_info,
						"Shutdown mount '%s' failed",
						info->mnt);
				ok = false;
			}
			D_FREE(info->mnt);
			D_FREE(info);
		}
	}
	return ok;
}

struct iof_barrier_info {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool in_barrier;
};

static void barrier_done(struct crt_barrier_cb_info *info)
{
	struct iof_barrier_info *b_info = info->bci_arg;

	if (info->bci_rc != 0)
		IOF_LOG_ERROR("Could not execute barrier: rc = %d\n",
			      info->bci_rc);

	D_MUTEX_LOCK(&b_info->lock);
	b_info->in_barrier = false;
	pthread_cond_signal(&b_info->cond);
	D_MUTEX_UNLOCK(&b_info->lock);
}

static void issue_barrier(void)
{
	struct iof_barrier_info b_info;

	pthread_mutex_init(&b_info.lock, NULL);
	pthread_cond_init(&b_info.cond, NULL);
	b_info.in_barrier = true;
	crt_barrier(NULL, barrier_done, &b_info);
	/* Existing service thread will progress barrier */
	D_MUTEX_LOCK(&b_info.lock);
	while (b_info.in_barrier)
		pthread_cond_wait(&b_info.cond, &b_info.lock);
	D_MUTEX_UNLOCK(&b_info.lock);

	pthread_cond_destroy(&b_info.cond);
	pthread_mutex_destroy(&b_info.lock);
}

/* Load a plugin from a fn pointer, return false if there was a fatal problem */
static bool
add_plugin(struct cnss_info *info,
	   cnss_plugin_init_t fn,
	   void *dl_handle)
{
	struct plugin_entry *entry;
	int rc;

	D_ALLOC_PTR(entry);
	if (!entry)
		return false;

	IOF_TRACE_UP(entry, info, "plugin_entry");

	rc = fn(&entry->fns, &entry->fns_size);
	if (rc != 0) {
		IOF_TRACE_INFO(entry, "Plugin at entry point %p failed (%d)",
			       FN_TO_PVOID(fn), rc);
		IOF_TRACE_DOWN(entry);
		D_FREE(entry);
		return false;
	}

	if (!entry->fns->name) {
		IOF_TRACE_ERROR(entry, "Disabling plugin: name is required\n");
		IOF_TRACE_DOWN(entry);
		D_FREE(entry);
		return false;
	}

	if (entry->fns->version != CNSS_PLUGIN_VERSION) {
		IOF_TRACE_ERROR(entry,
				"Plugin version incorrect %x %x, disabling",
				entry->fns->version,
				CNSS_PLUGIN_VERSION);
		IOF_TRACE_DOWN(entry);
		D_FREE(entry);
		return false;
	}

	rc = ctrl_create_subdir(NULL, entry->fns->name,
				&entry->self_fns.plugin_dir);
	if (rc != 0) {
		IOF_TRACE_ERROR(entry,
				"ctrl dir creation failed (%d), disabling", rc);
		IOF_TRACE_DOWN(entry);
		D_FREE(entry);
		return false;
	}

	IOF_TRACE_UP(entry->fns->handle, info, entry->fns->name);

	entry->self_fns.prefix = info->prefix;
	entry->active = true;

	entry->dl_handle = dl_handle;

	entry->self_fns.fuse_version = 3;

	entry->self_fns.get_config_option = get_config_option;
	entry->self_fns.create_ctrl_subdir = ctrl_create_subdir;
	entry->self_fns.register_ctrl_variable = ctrl_register_variable;
	entry->self_fns.register_ctrl_event = ctrl_register_event;
	entry->self_fns.register_ctrl_tracker = ctrl_register_tracker;
	entry->self_fns.register_ctrl_constant = ctrl_register_constant;
	entry->self_fns.register_ctrl_constant_int64 =
		ctrl_register_constant_int64;
	entry->self_fns.register_ctrl_constant_uint64 =
		ctrl_register_constant_uint64;
	entry->self_fns.register_ctrl_uint64_variable =
		ctrl_register_uint64_variable;
	entry->self_fns.register_fuse_fs = register_fuse;
	entry->self_fns.handle = entry;

	d_list_add(&entry->list, &info->plugins);

	D_INIT_LIST_HEAD(&entry->fuse_list);

	IOF_LOG_INFO("Added plugin %s(%p) from entry point %p ",
		     entry->fns->name,
		     (void *)entry->fns->handle,
		     FN_TO_PVOID(fn));

	if (sizeof(struct cnss_plugin) != entry->fns_size)
		IOF_TRACE_WARNING(entry->fns->handle,
				  "Plugin size incorrect %zd %zd, some functions may be disabled",
				  entry->fns_size,
				  sizeof(struct cnss_plugin));

	return true;
}

static void show_help(const char *prog)
{
	printf("I/O Forwarding Compute Node System Services\n");
	printf("\n");
	printf("Usage: %s [OPTION] ...\n", prog);
	printf("\n");
	printf("\t-h, --help\tThis help text\n");
	printf("\t-v, --version\tShow version\n");
	printf("\t-p, --prefix\tPath to the CNSS Working directory.\n"
		"\t\t\tThis may also be set via the CNSS_PREFIX"
		" environment variable.\n"
		"\n");
}

int main(int argc, char **argv)
{
	char *cnss = "CNSS";
	char *plugin_file = NULL;
	const char *prefix = NULL;
	char *version = iof_get_version();
	struct plugin_entry *list_iter;
	struct plugin_entry *list_iter_next;
	struct cnss_info *cnss_info;
	bool active_plugins = false;
	int ret;

	bool service_process_set = false;
	char *ctrl_prefix;
	bool rcb;

	iof_log_init("CN", "CNSS", NULL);

	IOF_LOG_INFO("CNSS version: %s", version);

	while (1) {
		static struct option long_options[] = {
			{"help", no_argument, 0, 'h'},
			{"version", no_argument, 0, 'v'},
			{"prefix", required_argument, 0, 'p'},
			{0, 0, 0, 0}
		};
		char c = getopt_long(argc, argv, "hvp:", long_options, NULL);

		if (c == -1)
			break;

		switch (c) {
		case 'h':
			show_help(argv[0]);
			exit(0);
			break;
		case 'v':
			printf("%s: %s\n", argv[0], version);
			exit(0);
			break;

		case 'p':
			prefix = optarg;
			break;
		case '?':
			exit(1);
			break;
		}
	}

	if (prefix == NULL) {
		prefix = getenv("CNSS_PREFIX");
	} else {
		ret = setenv("CNSS_PREFIX", prefix, 1);
		if (ret) {
			IOF_LOG_ERROR("setenv failed for "
				      "CNSS_PREFIX, rc %d", ret);
			return CNSS_ERR_PREFIX;
		}
	}
	if (prefix == NULL) {
		IOF_LOG_ERROR("CNSS prefix is required");
		return CNSS_ERR_PREFIX;
	}

	/* chdir to the cnss_prefix, as that allows all future I/O access
	 * to use relative paths.
	 */
	ret = chdir(prefix);
	if (ret != 0) {
		IOF_LOG_ERROR("Could not chdir to CNSS_PREFIX");
		return CNSS_ERR_PREFIX;
	}

	ret = crt_group_config_path_set(prefix);
	if (ret != 0) {
		IOF_LOG_ERROR("Could not set group config prefix");
		return CNSS_ERR_CART;
	}

	D_ALLOC_PTR(cnss_info);
	if (!cnss_info)
		return CNSS_ERR_NOMEM;
	IOF_TRACE_ROOT(cnss_info, "cnss_info");

	rcb = ctrl_info_init(&cnss_info->info);
	if (!rcb)
		return CNSS_ERR_PTHREAD;
	cnss_info->prefix = prefix;

	D_ASPRINTF(ctrl_prefix, "%s/.ctrl", prefix);
	if (!ctrl_prefix)
		return CNSS_ERR_NOMEM;

	ret = ctrl_fs_start(ctrl_prefix);
	if (ret != 0) {
		IOF_TRACE_ERROR(cnss_info, "Could not start ctrl fs");
		return CNSS_ERR_CTRL_FS;
	}

	register_cnss_controls(&cnss_info->info);

	D_INIT_LIST_HEAD(&cnss_info->plugins);

	if (getenv("CNSS_DISABLE_IOF") != NULL) {
		IOF_TRACE_INFO(cnss_info, "Skipping IOF plugin");
	} else {
		/* Load the built-in iof "plugin" */

		IOF_TRACE_INFO(cnss_info,
			       "Loading plugin at entry point %p",
			       FN_TO_PVOID(iof_plugin_init));

		rcb = add_plugin(cnss_info, iof_plugin_init, NULL);
		if (!rcb)
			D_GOTO(shutdown_ctrl_fs, ret = CNSS_ERR_PLUGIN);
	}

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

		IOF_TRACE_INFO(cnss_info,
			       "Loading plugin file %s %p %p",
			       plugin_file,
			       dl_handle,
			       FN_TO_PVOID(fn));
		if (fn) {
			rcb = add_plugin(cnss_info, fn, dl_handle);
			if (!rcb)
				D_GOTO(shutdown_ctrl_fs, ret = CNSS_ERR_PLUGIN);
		}
	}

	/* Walk the list of plugins and if any require the use of a service
	 * process set across the CNSS nodes then create one
	 */
	d_list_for_each_entry(list_iter, &cnss_info->plugins, list) {
		if (list_iter->active && list_iter->fns->require_service) {
			service_process_set = true;
			break;
		}
	}

	IOF_TRACE_INFO(cnss_info,
		       "Forming %s process set",
		       service_process_set ? "service" : "client");

	/*initialize CaRT*/
	ret = crt_init(cnss, service_process_set ? CRT_FLAG_BIT_SERVER : 0);
	if (ret) {
		IOF_TRACE_ERROR(cnss_info,
				"crt_init failed with ret = %d", ret);
		ret = CNSS_ERR_CART;
		goto shutdown_ctrl_fs;
	}

	if (service_process_set) {
		/* Need to dump the CNSS attach info for singleton
		 * CNSS clients (e.g. libcppr)
		 */
		ret = crt_group_config_save(NULL, false);
		if (ret != 0) {
			IOF_TRACE_ERROR(cnss_info,
					"Could not save attach info for CNSS");
			ret = CNSS_ERR_CART;
			goto shutdown_ctrl_fs;
		}
	}

	/* Call start for each plugin which should perform none-local
	 * operations only.  Plugins can choose to disable themselves
	 * at this point.
	 */
	CALL_PLUGIN_FN_START(&cnss_info->plugins, start);

	/* Wait for all nodes to finish 'start' before doing 'post_start' */
	if (service_process_set)
		issue_barrier();

	/* Call post_start for each plugin, which could communicate over
	 * the network.  Plugins can choose to disable themselves
	 * at this point.
	 */
	CALL_PLUGIN_FN_CHECK(&cnss_info->plugins, post_start);

	/* Walk the plugins and check for active ones */
	d_list_for_each_entry_safe(list_iter, list_iter_next,
				   &cnss_info->plugins, list) {
		if (list_iter->active) {
			active_plugins = true;
			continue;
		}
		if (list_iter->fns->destroy_plugin_data) {
			IOF_TRACE_INFO(cnss_info,
				       "Plugin %s(%p) calling destroy_plugin_data at %p",
				       list_iter->fns->name,
				       list_iter->fns->handle,
				       FN_TO_PVOID(list_iter->fns->destroy_plugin_data));
			list_iter->fns->destroy_plugin_data(list_iter->fns->handle);
		}
		d_list_del(&list_iter->list);

		if (list_iter->dl_handle)
			dlclose(list_iter->dl_handle);
		D_FREE(list_iter);
	}

	/* TODO: How to handle this case? */
	if (!active_plugins) {
		IOF_TRACE_ERROR(cnss_info, "No active plugins");
		ret = 1;
		goto shutdown_cart;
	}

	cnss_info->info.active = 1;

	wait_for_shutdown(&cnss_info->info);

	CALL_PLUGIN_FN(&cnss_info->plugins, stop_client_services);
	CALL_PLUGIN_FN(&cnss_info->plugins, flush_client_services);

	flush_fs(cnss_info);

	if (service_process_set)
		issue_barrier();

	CALL_PLUGIN_FN(&cnss_info->plugins, stop_plugin_services);
	CALL_PLUGIN_FN(&cnss_info->plugins, flush_plugin_services);

shutdown_cart:
	rcb = shutdown_fs(cnss_info);

	CALL_PLUGIN_FN(&cnss_info->plugins, destroy_plugin_data);

	ret = crt_finalize();
	if (!rcb)
		ret = 1;
	ctrl_fs_shutdown(); /* Shuts down ctrl fs and waits */

	while (!d_list_empty(&cnss_info->plugins)) {
		struct plugin_entry *entry;

		entry = d_list_entry(cnss_info->plugins.next,
				     struct plugin_entry, list);
		d_list_del(&entry->list);

		if (entry->dl_handle != NULL)
			dlclose(entry->dl_handle);
		IOF_TRACE_DOWN(entry);
		D_FREE(entry);
	}

	IOF_TRACE_DOWN(cnss_info);
	D_FREE(ctrl_prefix);

	IOF_TRACE_INFO(cnss_info, "Exiting with status %d", ret);

	D_FREE(cnss_info);

	iof_log_close();

	return ret;

shutdown_ctrl_fs:
	ctrl_fs_disable();
	ctrl_fs_shutdown();
	D_FREE(ctrl_prefix);

	IOF_LOG_INFO("Exiting with status %d", ret);

	return ret;
}

int cnss_dump_log(struct ctrl_info *info)
{
	struct cnss_info *cnss_info = container_of(info, struct cnss_info,
						   info);

	if (!cnss_info)
		return -1;

	CALL_PLUGIN_FN(&cnss_info->plugins, dump_log);
	return 0;
}

