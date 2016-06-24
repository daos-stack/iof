/* Users of this plugin interface should copy the file from
 * <iof_prefix>/share/plugin to their source tree so to ensure
 * a forward compatible plugin
 */
#include <unistd.h>

#ifndef __IOF_PLUGIN_H__
#define __IOF_PLUGIN_H__

#if defined(__cplusplus)
extern "C" {
#endif

struct fuse_operations;
struct mcl_state;

/* Function lookup table provided by CNSS to plugin */
struct cnss_plugin_cb {
	void *handle;
	int fuse_version;
	const char *(*get_config_option)(const char *); /* A wrapper
							 * around getenv
							 */

	/* Startup FUSE clients ? */
	void *(*register_fuse_fs)(void *handle, struct fuse_operations*,
				  const char *);
	int (*deregister_fuse_fs)(void *handle, void *);

	/* CPPR needs to be able to access the "global file system" so needs
	 * to enumerate over projection to be able to pick a destination and
	 * then access the struct fs_ops structure to be able to write to it
	 */
};


/* Function lookup table provided by plugin to CNSS. */
struct cnss_plugin {
	void *handle;  /** Handle passed back to all callback functions */
	int require_service; /** Does the plugin need CNSS to be a service
			      *  process set
			      */
	int (*start)(void *, struct mcl_state *, struct cnss_plugin_cb *,
		     size_t); /* Called once at startup, should return 0 */
	void (*client_attached)(void *, int); /* Notify plugin of a new
					       * local process
					       */
	void (*client_detached)(void *, int); /* Notify plugin of local
					       * process removal
					       */

	void (*flush)(void *); /* Commence shutdown procedure */
	void (*finish)(void *); /* Shutdown, free all memory before returning */
};

/* At startup the CNSS process loads every library in a predefined
 * directory, and looks for a cnss_plugin_init() function in that
 * library.  This function should pass out a struct cnss_plugin and
 * a size, and return 0 on success.
 */
typedef int (*cnss_plugin_init_t)(struct cnss_plugin **fns, size_t *size);

/* The name of the init symbol defined in the plugin library */
const char *cnss_plugin_init_symbol = "cnss_plugin_init";

/* Library (interception library or CPPR Library) needs function to "attach" to
 * local CNSS by opening file in ctrl filesystem and be able to detect network
 * address
 *
 * iof will need to install a shared library which IL and CPPR library can use.
 */

#if defined(__cplusplus)
}
#endif

#endif /* __IOF_PLUGIN_H__ */
