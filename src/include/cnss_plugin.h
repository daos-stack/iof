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
/* Users of this plugin interface should copy the file from
 * <iof_prefix>/share/plugin to their source tree so to ensure
 * a forward compatible plugin
 */
#include <unistd.h>

#ifndef __CNSS_PLUGIN_H__
#define __CNSS_PLUGIN_H__

#if defined(__cplusplus)
extern "C" {
#endif

#define CNSS_SUCCESS		0
#define CNSS_ERR_PREFIX		1 /*CNSS prefix is not set in the environment*/
#define CNSS_ERR_NOMEM		2 /*no memory*/
#define CNSS_ERR_PLUGIN		3 /*failed to load or initialize plugin*/
#define CNSS_ERR_FUSE		4 /*failed to register or deregister FUSE*/
#define CNSS_ERR_CART		5 /*CaRT failed*/
#define CNSS_BAD_DATA		6 /*bad data*/
#define CNSS_ERR_CTRL_FS	7 /*ctrl fs did not start or shutdown*/
#define CNSS_ERR_PTHREAD	8 /*failed to create or destroy CNSS threads*/


struct fuse_operations;

/* Optional callback invoked when a read is done on a ctrl fs variable */
typedef int (*ctrl_fs_read_cb_t)(char *buf, size_t buflen, void *cb_arg);
/* Optional callback invoked when a write is done on a ctrl fs variable */
typedef int (*ctrl_fs_write_cb_t)(const char *value, void *cb_arg);

/* Optional callback invoked when an open is done on a ctrl fs counter */
typedef int (*ctrl_fs_open_cb_t)(int value, void *cb_arg);
/* Optional callback invoked when a close is done on a ctrl fs counter */
typedef int (*ctrl_fs_close_cb_t)(int value, void *cb_arg);
/* Optional callback invoked when ctrl fs is shutting down */
typedef int (*ctrl_fs_destroy_cb_t)(void *cb_arg);
/* Optional callback invoked when a trigger is done on a ctrl fs event.
 * A trigger occurs on any modification to the underlying file.
 */
typedef int (*ctrl_fs_trigger_cb_t)(void *cb_arg);

/* Function lookup table provided by CNSS to plugin */
struct cnss_plugin_cb {
	void *handle;
	int fuse_version;
	const char *(*get_config_option)(const char *); /* A wrapper
							 * around getenv
							 */

	/* Launch FUSE mount.  Returns 0 on success */
	int (*register_fuse_fs)(void *handle, struct fuse_operations*,
				const char *, void *);

	/* Registers a variable, exported as a control file system file
	 * and associates optional callbacks with read and write events.
	 */
	int (*register_ctrl_variable)(const char *path,
				      ctrl_fs_read_cb_t read_cb,
				      ctrl_fs_write_cb_t write_cb,
				      ctrl_fs_destroy_cb_t destroy_cb,
				      void *cb_arg);
	/* Registers an event, exported as a control file system file
	 * and associates optional callbacks with change events.
	 */
	int (*register_ctrl_event)(const char *path,
				   ctrl_fs_trigger_cb_t trigger_cb,
				   ctrl_fs_destroy_cb_t destroy_cb,
				   void *cb_arg);
	/* Registers a counter, exported as a control file system file
	 * and associates optional callbacks with open/close events.
	 */
	int (*register_ctrl_counter)(const char *path, int start, int increment,
				     ctrl_fs_open_cb_t open_cb,
				     ctrl_fs_close_cb_t close_cb,
				     ctrl_fs_destroy_cb_t destroy_cb,
				     void *cb_arg);
	/*
	 * Control fs constant registration.  Output should be what you want
	 * to see when you cat <path>.
	 */
	int (*register_ctrl_constant)(const char *path, const char *output);

	/* CPPR needs to be able to access the "global file system" so needs
	 * to enumerate over projection to be able to pick a destination and
	 * then access the struct fs_ops structure to be able to write to it
	 */
};


/* Function lookup table provided by plugin to CNSS. */
struct cnss_plugin {
	int version; /** Set to CNSS_PLUGIN_VERSION for startup checks */
	int require_service; /** Does the plugin need CNSS to be a service
			      *  process set
			      */
	char *name;    /** Short string used to prefix log information */
	void *handle;  /** Handle passed back to all callback functions */
	int (*start)(void *, struct cnss_plugin_cb *,
		     size_t); /* Called once at startup, should return 0.
			       * If a non-zero code is returned then the plugin
			       * is disabled and no more callbacks are made.
			       */
	int (*post_start)(void *);
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
#define CNSS_PLUGIN_INIT_SYMBOL "cnss_plugin_init"

/* Runtime version checking.
 * The plugin must define .version to this value or it will be disabled at
 * runtime.
 *
 * Additionally, offsets of members within cnss_plugin are checked at runtime so
 * it is safe to expand the API by appending new members, whilst maintaining
 * binary compatibility, however if any members are moved to different offsets
 * or change parameters or meaning then change this version to force a
 * re-compile of existing plugins.
 */
#define CNSS_PLUGIN_VERSION 0x10f003

/* Library (interception library or CPPR Library) needs function to "attach" to
 * local CNSS by opening file in ctrl filesystem and be able to detect network
 * address
 *
 * iof will need to install a shared library which IL and CPPR library can use.
 */

#if defined(__cplusplus)
}
#endif

#endif /* __CNSS_PLUGIN_H__ */
