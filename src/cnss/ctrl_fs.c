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
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <sys/queue.h>
#ifdef __APPLE__
# include <sys/syslimits.h>
#define PATH_MAX MAXPATHLEN
#else /* !__APPLE__ */
# include <linux/limits.h>
#endif /* __APPLE__ */
#ifdef IOF_USE_FUSE3
# include <fuse3/fuse.h>
# include <fuse3/fuse_lowlevel.h>
#else
# include <fuse/fuse.h>
# include <fuse/fuse_lowlevel.h>
#endif

#include "log.h"
#include "ctrl_fs.h"

enum {
	CTRL_DIR = 0,
	CTRL_VARIABLE,
	CTRL_EVENT,
	CTRL_CONSTANT,
	CTRL_COUNTER,
	NUM_CTRL_TYPES,
};

#define CTRL_DATA_MAX 4096
#define CTRL_INT_MAX  11
#define CTRL_NAME_MAX 256

struct ctrl_variable {
	ctrl_fs_read_cb_t read_cb;
	ctrl_fs_write_cb_t write_cb;
	ctrl_fs_destroy_cb_t destroy_cb;
	void *cb_arg;
};

struct ctrl_event {
	ctrl_fs_trigger_cb_t trigger_cb;
	ctrl_fs_destroy_cb_t destroy_cb;
	void *cb_arg;
};

struct ctrl_constant {
	char buf[CTRL_DATA_MAX];
};

struct ctrl_counter {
	int next_value;
	int increment;
	void *cb_arg;
	ctrl_fs_open_cb_t open_cb;
	ctrl_fs_close_cb_t close_cb;
	ctrl_fs_destroy_cb_t destroy_cb;
};

union ctrl_data {
	struct ctrl_variable var;
	struct ctrl_event    evnt;
	struct ctrl_constant con;
	struct ctrl_counter  cnt;
};


TAILQ_HEAD(ctrl_node_queue, ctrl_node);

struct ctrl_node {
	char name[CTRL_NAME_MAX];
	TAILQ_ENTRY(ctrl_node) entry;
	struct ctrl_node_queue queue;
	pthread_rwlock_t lock;
	struct stat stat_info;
	int ctrl_type;
	int initialized;
};

struct data_node {
	struct ctrl_node node;
	union ctrl_data data[];
};

#define SET_DATA(node, type, field, value) \
	((struct data_node *)(node))->data[0].type.field = value

#define GET_DATA(node, type, field) \
	(((struct data_node *)(node))->data[0].type.field)

struct ctrl_fs_data {
	char *prefix;
	struct fuse *fuse;
	struct fuse_chan *ch;
	int next_inode;
	int startup_rc;
	pthread_t thread;
	struct ctrl_node root;
	bool started;
};

static pthread_once_t once_init = PTHREAD_ONCE_INIT;
static struct ctrl_fs_data ctrl_fs;

static int init_node(struct ctrl_node *node, const char *name,
		     int mode, size_t size)
{
	int inode = __sync_fetch_and_add(&ctrl_fs.next_inode, 1);
	time_t seconds;
	int rc;

	rc = pthread_rwlock_init(&node->lock, NULL);
	if (rc != 0) {
		IOF_LOG_ERROR("Could not initialize rwlock for ctrl node %s",
			      name);
		return rc;
	}

	strncpy(node->name, name, CTRL_NAME_MAX);
	node->name[CTRL_NAME_MAX - 1] = 0;
	TAILQ_INIT(&node->queue);

	seconds = time(NULL);
	node->stat_info.st_ctime = seconds;
	node->stat_info.st_atime = seconds;
	node->stat_info.st_mtime = seconds;
	node->stat_info.st_nlink = 1;
	node->stat_info.st_uid = getuid();
	node->stat_info.st_gid = getgid();
	node->stat_info.st_ino = inode;
	node->stat_info.st_mode = mode;
	node->stat_info.st_size = size;

	return 0;
}

static void init_root_node(void)
{
	int rc;

	rc = init_node(&ctrl_fs.root, "", S_IFDIR | S_IRWXU, 0);

	if (rc != 0) {
		IOF_LOG_ERROR("Could not init control file system (rc = %d)",
			      rc);
		ctrl_fs.startup_rc = rc;
	}
}

static int allocate_node(struct ctrl_node **node, const char *name,
			 int mode, int ctrl_type)
{
	struct ctrl_node *newnode = NULL;
	int rc;
	size_t size = CTRL_DATA_MAX;
	size_t dsize = 0;

	*node = NULL;

	switch (ctrl_type) {
	case CTRL_VARIABLE:
		dsize = sizeof(struct ctrl_variable);
		break;
	case CTRL_EVENT:
		dsize = sizeof(struct ctrl_event);
		break;
	case CTRL_CONSTANT:
		dsize = sizeof(struct ctrl_constant);
		break;
	case CTRL_COUNTER:
		dsize = sizeof(struct ctrl_counter);
		break;
	default:
		size = 0; /* No data */
		break;
	}

	/* Ok, go ahead and allocate a new node, assuming no conflict */
	newnode = (struct ctrl_node *)calloc(1,
					     sizeof(struct ctrl_node) + dsize);

	if (newnode == NULL) {
		IOF_LOG_ERROR("Not enough memory to allocate ctrl node");
		return -ENOMEM;
	}

	rc = init_node(newnode, name, mode, size);

	if (rc != 0) {
		IOF_LOG_ERROR("Could not initialize ctrl node %s", name);
		free(newnode);
		return rc;
	}

	newnode->ctrl_type = ctrl_type;

	*node = newnode;
	return 0;
}

static int free_node(struct ctrl_node *node);

static int cleanup_node(struct ctrl_node *node)
{
	struct ctrl_node *item;
	void *cb_arg;
	ctrl_fs_destroy_cb_t destroy_cb = NULL;
	int rc;
	int bad_rc;


	rc = pthread_rwlock_destroy(&node->lock);

	if (rc != 0) {
		IOF_LOG_ERROR("Could not destroy rwlock in ctrl node");
		return rc;
	}

	while (!TAILQ_EMPTY(&node->queue)) {
		item = TAILQ_FIRST(&node->queue);
		TAILQ_REMOVE(&node->queue, item, entry);
		if (node->ctrl_type == CTRL_VARIABLE) {
			cb_arg = GET_DATA(node, var, cb_arg);
			destroy_cb = GET_DATA(node, var, destroy_cb);
		} else if (node->ctrl_type == CTRL_EVENT) {
			cb_arg = GET_DATA(node, evnt, cb_arg);
			destroy_cb = GET_DATA(node, evnt, destroy_cb);
		} else if (node->ctrl_type == CTRL_COUNTER) {
			cb_arg = GET_DATA(node, cnt, cb_arg);
			destroy_cb = GET_DATA(node, cnt, destroy_cb);
		}
		if (destroy_cb != NULL) {
			bad_rc = destroy_cb(cb_arg);
			if (bad_rc != 0) {
				IOF_LOG_ERROR("Error destroying ctrl node %s",
					      node->name);
				/* Save the value but don't exit the loop */
				rc = bad_rc;
			}
		}
		bad_rc = free_node(item);

		if (bad_rc != 0) {
			IOF_LOG_ERROR("Could not clean child ctrl nodes %s",
				      node->name);
			/* Save the value but don't exit the loop */
			rc = bad_rc;
		}
	}

	return rc;
}

static int free_node(struct ctrl_node *node)
{
	int rc;

	rc = cleanup_node(node);

	if (rc != 0)
		IOF_LOG_ERROR("Could not clean ctrl node %s", node->name);

	free(node);

	return rc;
}

static int find_node(struct ctrl_node *parent, struct ctrl_node **node,
		     const char *name, bool lock_held)
{
	int rc = 0;
	struct ctrl_node *item;

	*node = NULL;

	if (!lock_held) {
		rc = pthread_rwlock_rdlock(&parent->lock);
		if (rc != 0) {
			IOF_LOG_ERROR("Could not acquire lock on ctrl node %s",
				      parent->name);
			return rc;
		}
	}

	TAILQ_FOREACH(item, &parent->queue, entry) {
		if (strncmp(name, item->name, CTRL_NAME_MAX) == 0)
			break;
	}

	if (!lock_held) {
		rc = pthread_rwlock_unlock(&parent->lock);
		if (rc != 0) {
			IOF_LOG_ERROR("Could not release lock on ctrl node %s",
				      parent->name);
			return rc;
		}
	}

	*node = item;

	return 0;
}

static int insert_node(struct ctrl_node *parent, struct ctrl_node *child)
{
	int rc = 0;
	struct ctrl_node *node;

	pthread_rwlock_wrlock(&parent->lock);

	rc = find_node(parent, &node, child->name, true);

	if (rc != 0) {
		IOF_LOG_ERROR("Error while searching for ctrl node");
		goto out;
	}

	if (node != NULL) {/* file/directory already exists */
		if (node->ctrl_type != CTRL_DIR ||
		    child->ctrl_type != CTRL_DIR) {
			IOF_LOG_ERROR("Conflict trying to add %s to ctrl_fs",
				      child->name);
			rc = -EEXIST;
		}
		free_node(child); /* Node conflict so node is not needed */
		goto out;
	}

	TAILQ_INSERT_TAIL(&parent->queue, child, entry);
out:
	pthread_rwlock_unlock(&parent->lock);

	return rc;

}

static int add_ctrl_dir(const char *name, struct ctrl_node **node)
{
	int rc = 0;
	struct ctrl_node *parent = *node;
	struct ctrl_node *newnode = NULL;
	struct ctrl_node *item;

	rc = find_node(parent, &item, name, false);

	if (rc != 0) {
		IOF_LOG_ERROR("Error while searching for ctrl node");
		return rc;
	}

	if (item != NULL) {/* file/directory already exists */
		if (item->ctrl_type != CTRL_DIR) {
			IOF_LOG_ERROR("Conflict trying to add %s to ctrl_fs",
				      name);
			return -EEXIST;
		}
		*node = item;
		return 0;
	}

	rc = allocate_node(&newnode, name, S_IFDIR | S_IRUSR | S_IXUSR,
			   CTRL_DIR);
	if (rc != 0) {
		IOF_LOG_ERROR("Could not allocate ctrl node %s", name);
		return rc;
	}

	rc = insert_node(parent, newnode);

	if (rc != 0) {
		IOF_LOG_ERROR("Could not insert ctrl node %s", name);
		return rc;
	}

	newnode->initialized = 1;
	*node = newnode;

	return 0;
}

static int add_ctrl_file(const char *name, struct ctrl_node **node,
			 int mode, int ctrl_type)
{
	int rc = 0;
	struct ctrl_node *parent = *node;
	struct ctrl_node *newnode = NULL;
	struct ctrl_node *item;

	rc = find_node(parent, &item, name, false);

	if (rc != 0) {
		IOF_LOG_ERROR("Error while searching for ctrl node");
		return rc;
	}

	if (item != NULL) {/* file/directory already exists */
		IOF_LOG_ERROR("Conflict trying to add %s to ctrl_fs",
			      name);
		return -EEXIST;
	}

	rc = allocate_node(&newnode, name, mode, ctrl_type);
	if (rc != 0) {
		IOF_LOG_ERROR("Could not allocate ctrl node %s", name);
		return rc;
	}

	rc = insert_node(parent, newnode);

	if (rc != 0) {
		IOF_LOG_ERROR("Could not insert ctrl node %s", name);
		return rc;
	}

	*node = newnode;

	return 0;

}

int add_intermediate_nodes(char *path, const char **basename,
			   struct ctrl_node **parent)
{
	char *cursor;
	char *next_token;
	char *token;
	int rc;
	struct ctrl_node *node = &ctrl_fs.root;

	*parent = NULL;
	*basename = NULL;

	token = strtok_r(path, "/", &cursor);

	if (token == NULL) {
		IOF_LOG_ERROR("Badly formatted path for ctrl variable %s",
			      path);
		return -EINVAL;
	}

	next_token = strtok_r(NULL, "/", &cursor);
	while (next_token != NULL) {
		/* Intermediate node */
		rc = add_ctrl_dir(token, &node);
		if (rc != 0) {
			IOF_LOG_ERROR("Error adding intermediate ctrl node %s",
				      token);
			return rc;
		}
		token = next_token;
		next_token = strtok_r(NULL, "/", &cursor);
	}

	*basename = token;
	*parent = node;

	return 0;
}

int ctrl_register_variable(const char *path, ctrl_fs_read_cb_t read_cb,
			   ctrl_fs_write_cb_t write_cb,
			   ctrl_fs_destroy_cb_t destroy_cb, void *cb_arg)
{
	char *scratch = NULL;
	const char *basename;
	struct ctrl_node *node;
	int rc = 0;
	int mode = S_IFREG;

	pthread_once(&once_init, init_root_node);

	if (ctrl_fs.startup_rc != 0)
		return ctrl_fs.startup_rc;

	if (path == NULL) {
		IOF_LOG_ERROR("Invalid path specified for ctrl variable");
		return -EINVAL;
	}

	scratch = strdup(path);
	if (scratch == NULL) {
		IOF_LOG_ERROR("No memory to register ctrl variable %s", path);
		return -ENOMEM;
	}

	rc = add_intermediate_nodes(scratch, &basename, &node);
	if (rc != 0) {
		IOF_LOG_ERROR("Error adding intermediate ctrl nodes %s", path);
		goto out;
	}

	if (read_cb != NULL)
		mode |= S_IRUSR;
	if (write_cb != NULL)
		mode |= S_IWUSR;

	rc = add_ctrl_file(basename, &node, mode, CTRL_VARIABLE);

	if (rc != 0)
		IOF_LOG_ERROR("Bad file %s specified in %s", basename, path);

	SET_DATA(node, var, cb_arg, cb_arg);
	SET_DATA(node, var, read_cb, read_cb);
	SET_DATA(node, var, destroy_cb, destroy_cb);
	SET_DATA(node, var, write_cb, write_cb);

	__sync_synchronize();
	node->initialized = 1;

	IOF_LOG_INFO("Registered %s as ctrl variable", path);
out:
	if (scratch != NULL)
		free(scratch);

	return rc;
}

int ctrl_register_event(const char *path, ctrl_fs_trigger_cb_t trigger_cb,
			ctrl_fs_destroy_cb_t destroy_cb, void *cb_arg)
{
	char *scratch = NULL;
	const char *basename;
	struct ctrl_node *node;
	int rc = 0;
	int mode = S_IFREG | S_IWUSR;

	pthread_once(&once_init, init_root_node);

	if (ctrl_fs.startup_rc != 0)
		return ctrl_fs.startup_rc;

	if (path == NULL) {
		IOF_LOG_ERROR("Invalid path specified for ctrl event");
		return -EINVAL;
	}

	scratch = strdup(path);
	if (scratch == NULL) {
		IOF_LOG_ERROR("No memory to register ctrl event %s", path);
		return -ENOMEM;
	}

	rc = add_intermediate_nodes(scratch, &basename, &node);
	if (rc != 0) {
		IOF_LOG_ERROR("Error adding intermediate ctrl nodes %s", path);
		goto out;
	}

	rc = add_ctrl_file(basename, &node, mode, CTRL_EVENT);

	if (rc != 0)
		IOF_LOG_ERROR("Bad file %s specified in %s", basename, path);

	SET_DATA(node, evnt, cb_arg, cb_arg);
	SET_DATA(node, evnt, trigger_cb, trigger_cb);
	SET_DATA(node, evnt, destroy_cb, destroy_cb);

	__sync_synchronize();
	node->initialized = 1;

	IOF_LOG_INFO("Registered %s as ctrl event", path);
out:
	if (scratch != NULL)
		free(scratch);

	return rc;
}

int ctrl_register_constant(const char *path, const char *value)
{
	char *scratch = NULL;
	const char *basename;
	struct ctrl_node *node;
	int rc = 0;

	pthread_once(&once_init, init_root_node);

	if (ctrl_fs.startup_rc != 0)
		return ctrl_fs.startup_rc;

	if (path == NULL) {
		IOF_LOG_ERROR("Invalid path specified for ctrl constant");
		return -EINVAL;
	}

	if (value == NULL) {
		IOF_LOG_ERROR("Invalid value specified for ctrl constant");
		return -EINVAL;
	}

	scratch = strdup(path);
	if (scratch == NULL) {
		IOF_LOG_ERROR("No memory to register ctrl variable %s", path);
		return -ENOMEM;
	}

	rc = add_intermediate_nodes(scratch, &basename, &node);
	if (rc != 0) {
		IOF_LOG_ERROR("Error adding intermediate ctrl nodes %s", path);
		goto out;
	}

	rc = add_ctrl_file(basename, &node, S_IFREG | S_IRUSR, CTRL_CONSTANT);

	if (rc != 0)
		IOF_LOG_ERROR("Bad file %s specified in %s", basename, path);

	strncpy(GET_DATA(node, con, buf), value, CTRL_DATA_MAX);
	__sync_synchronize();
	node->initialized = 1;

	IOF_LOG_INFO("Registered %s as ctrl constant.  Value is %s",
		     path, value);
out:
	if (scratch != NULL)
		free(scratch);

	return rc;
}

int ctrl_register_counter(const char *path, int start, int increment,
			  ctrl_fs_open_cb_t open_cb,
			  ctrl_fs_close_cb_t close_cb,
			  ctrl_fs_destroy_cb_t destroy_cb,
			  void *cb_arg)
{
	char *scratch = NULL;
	const char *basename;
	struct ctrl_node *node;
	int rc = 0;

	pthread_once(&once_init, init_root_node);

	if (ctrl_fs.startup_rc != 0)
		return ctrl_fs.startup_rc;

	if (path == NULL) {
		IOF_LOG_ERROR("Invalid path specified for ctrl variable");
		return -EINVAL;
	}

	scratch = strdup(path);
	if (scratch == NULL) {
		IOF_LOG_ERROR("No memory to register ctrl variable %s", path);
		return -ENOMEM;
	}

	rc = add_intermediate_nodes(scratch, &basename, &node);
	if (rc != 0) {
		IOF_LOG_ERROR("Error adding intermediate ctrl nodes %s", path);
		goto out;
	}

	rc = add_ctrl_file(basename, &node, S_IFREG | S_IRUSR, CTRL_COUNTER);

	if (rc != 0)
		IOF_LOG_ERROR("Bad file %s specified in %s", basename, path);

	SET_DATA(node, cnt, cb_arg, cb_arg);
	SET_DATA(node, cnt, open_cb, open_cb);
	SET_DATA(node, cnt, close_cb, close_cb);
	SET_DATA(node, cnt, destroy_cb, destroy_cb);
	SET_DATA(node, cnt, next_value, start);
	SET_DATA(node, cnt, increment, increment);

	__sync_synchronize();
	node->initialized = 1;

	IOF_LOG_INFO("Registered %s as ctrl counter (%d, %d)",
		     path, start, increment);
out:
	if (scratch != NULL)
		free(scratch);

	return rc;
}

static void *ctrl_thread_func(void *arg)
{
	int rc;

	IOF_LOG_INFO("Starting ctrl fs loop");

	rc = fuse_loop(ctrl_fs.fuse); /* Blocking */

	IOF_LOG_INFO("Exited ctrl fs loop");

	if (rc != 0)
		IOF_LOG_ERROR("Fuse loop exited with %d", rc);
	if (ctrl_fs.ch != NULL)
#ifdef IOF_USE_FUSE3
		fuse_unmount(ctrl_fs.fuse);
#else
		fuse_unmount(ctrl_fs.prefix, ctrl_fs.ch);
#endif
	if (ctrl_fs.fuse != NULL)
		fuse_destroy(ctrl_fs.fuse);

	return NULL;
}

static void cleanup_ctrl_fs(void)
{
	IOF_LOG_INFO("Cleaning up ctrl fs");
	free(ctrl_fs.prefix);
}

static int find_path_node(const char *path, struct ctrl_node **node)
{
	char buf[PATH_MAX];
	char *token;
	char *cursor;
	struct ctrl_node *current_node;
	int rc;

	if (strcmp(path, "/") == 0) {
		*node = &ctrl_fs.root;
		return 0;
	}

	current_node = &ctrl_fs.root;

	strncpy(buf, path, PATH_MAX);
	buf[PATH_MAX - 1] = 0;

	token = strtok_r(buf, "/", &cursor);

	current_node = &ctrl_fs.root;
	while (token != NULL) {
		rc = find_node(current_node, &current_node, token, false);

		if (rc != 0)
			return rc;

		token = strtok_r(NULL, "/", &cursor);
	}

	*node = current_node;

	return 0;
}

#ifdef IOF_USE_FUSE3
static int ctrl_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi,
		enum fuse_readdir_flags flags)
#else
static int ctrl_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			off_t offset, struct fuse_file_info *fi)
#endif
{
	struct ctrl_node *node;
	struct ctrl_node *item;
	int rc;

	if (!ctrl_fs.started)
		return -ENOENT;

	IOF_LOG_INFO("ctrl_fs readdir called for %s", path);

	rc = find_path_node(path, &node);

	if (rc != 0 || node == NULL)
		return -ENOENT;

	if (node->ctrl_type != CTRL_DIR)
		return -ENOTDIR;

	pthread_rwlock_rdlock(&node->lock);
	/* There doesn't seem to be an appropriate readdir error code if this
	 * fails.   So I guess let it race, I guess.
	 */

	TAILQ_FOREACH(item, &node->queue, entry) {
		if (item->initialized == 0)
			continue;

		if (filler(buf, item->name, &item->stat_info, 0
#ifdef IOF_USE_FUSE3
			   , flags
#endif
			   ))
			break;
	}

	pthread_rwlock_unlock(&node->lock);

	return 0;
}

static int ctrl_getattr(const char *fname, struct stat *stat)
{
	struct ctrl_node *node;
	int rc;

	IOF_LOG_INFO("ctrl_fs getattr called for %s", fname);

	rc = find_path_node(fname, &node);

	if (rc != 0 || node == NULL)
		return -ENOENT;

	IOF_LOG_INFO("Returning getattr for '%s' mode = %x", node->name,
		     node->stat_info.st_mode >> 2);
	memcpy(stat, &node->stat_info, sizeof(struct stat));

	return 0;
}

static int ctrl_open(const char *fname, struct fuse_file_info *finfo)
{
	struct ctrl_node *node;
	int rc;
	bool read_access = false;
	bool write_access = false;

	if (!ctrl_fs.started)
		return 0;

	IOF_LOG_INFO("ctrl fs open called for %s", fname);

	rc = find_path_node(fname, &node);

	if (rc != 0 || node == NULL || node->initialized == 0)
		return -ENOENT;

	if ((finfo->flags & O_RDWR) == O_RDWR)
		read_access = write_access = true;
	else if ((finfo->flags & (O_WRONLY|O_CREAT|O_TRUNC)) != 0)
		write_access = true;
	else
		read_access = true;

	if (read_access && ((node->stat_info.st_mode & S_IRUSR) == 0)) {
		IOF_LOG_DEBUG("Could not open %s due to read permissions",
			      fname);
		return -EPERM;
	}

	if (write_access && ((node->stat_info.st_mode & S_IWUSR) == 0)) {
		IOF_LOG_DEBUG("Could not open %s due to write permissions",
			      fname);
		return -EPERM;
	}

	finfo->fh = 0;

	if (node->ctrl_type == CTRL_COUNTER) {
		int count;
		int *next_value = &GET_DATA(node, cnt, next_value);
		int increment = GET_DATA(node, cnt, increment);
		ctrl_fs_open_cb_t open_cb;
		void *cb_arg = GET_DATA(node, cnt, cb_arg);

		open_cb = GET_DATA(node, cnt, open_cb);
		count = __sync_fetch_and_add(next_value, increment);

		if (open_cb != NULL)
			open_cb(count, cb_arg);

		finfo->fh = count;
	}

	/* Nothing to do for EVENT, VARIABLE, or CONSTANT on open */

	return 0;
}

static int ctrl_truncate(const char *fname, off_t size)
{
	struct ctrl_node *node;
	int rc;

	if (!ctrl_fs.started)
		return 0;

	IOF_LOG_INFO("ctrl fs truncate called for %s", fname);

	rc = find_path_node(fname, &node);

	if (rc != 0 || node == NULL || node->initialized == 0)
		return -ENOENT;

	return 0;
}

static int ctrl_read(const char *fname,
		     char *buf,
		     size_t size,
		     off_t offset,
		     struct fuse_file_info *finfo)
{
	char mybuf[CTRL_DATA_MAX];
	struct ctrl_node *node;
	const char *payload;
	size_t len;
	int rc;

	if (!ctrl_fs.started)
		return 0;

	IOF_LOG_INFO("ctrl fs read called for %s", fname);

	rc = find_path_node(fname, &node);

	if (rc != 0 || node == NULL || node->initialized == 0)
		return -ENOENT;

	memset(buf, 0, size);

	if (node->ctrl_type == CTRL_CONSTANT)
		payload = GET_DATA(node, con, buf);
	else if (node->ctrl_type == CTRL_VARIABLE) {
		ctrl_fs_read_cb_t read_cb;
		void *cb_arg = GET_DATA(node, var, cb_arg);

		read_cb = GET_DATA(node, var, read_cb);

		if (read_cb != NULL) {
			rc = read_cb(mybuf, CTRL_DATA_MAX, cb_arg);
			if (rc != 0) {
				IOF_LOG_ERROR("Error reading ctrl variable");
				return -ENOENT;
			}
		}
		payload = mybuf;
	} else if (node->ctrl_type == CTRL_COUNTER) {
		sprintf(mybuf, "%d", (int)finfo->fh);
		payload = mybuf;
	}

	len = snprintf(buf, size, "%s\n", payload);
	if (len >= size) {
		len = size;
		IOF_LOG_WARNING("Truncated value for %s", fname);
		buf[size - 1] = '\n';
	}

	IOF_LOG_INFO("Done copying contents to output buffer %s len is %ld",
		     fname, len);

	return len;
}

static int ctrl_mknod(const char *path,
		      mode_t mode,
		      dev_t rdev)
{
	struct ctrl_node *node;
	int rc;

	if (!ctrl_fs.started)
		return 0;

	IOF_LOG_INFO("ctrl fs mknod called for %s", path);

	rc = find_path_node(path, &node);

	if (rc != 0 || node == NULL || node->initialized == 0)
		return -ENOENT;

	return 0;
}

static int ctrl_write(const char *fname,
		      const char *buf,
		      size_t len,
		      off_t offset,
		      struct fuse_file_info *finfo)
{
	char mybuf[CTRL_DATA_MAX];
	struct ctrl_node *node;
	int rc;

	if (!ctrl_fs.started)
		return 0;

	IOF_LOG_INFO("ctrl fs write called for %s", fname);

	rc = find_path_node(fname, &node);

	if (rc != 0 || node == NULL || node->initialized == 0)
		return -ENOENT;

	if (node->ctrl_type == CTRL_EVENT) {
		ctrl_fs_trigger_cb_t trigger_cb;
		void *cb_arg = GET_DATA(node, evnt, cb_arg);

		trigger_cb = GET_DATA(node, evnt, trigger_cb);

		if (trigger_cb != NULL) {
			trigger_cb(cb_arg);
			if (rc != 0) {
				IOF_LOG_ERROR("Error triggering ctrl variable");
				return -ENOENT;
			}
		}
	} else if (node->ctrl_type == CTRL_VARIABLE) {
		ctrl_fs_write_cb_t write_cb;
		void *cb_arg = GET_DATA(node, var, cb_arg);

		write_cb = GET_DATA(node, var, write_cb);

		if (write_cb != NULL) {
			size_t mylen = len;

			if (len > (CTRL_DATA_MAX - 1))
				mylen = CTRL_DATA_MAX - 1;

			memcpy(mybuf, buf, mylen);
			mybuf[mylen] = 0;
			rc = write_cb(mybuf, cb_arg);
			if (rc != 0) {
				IOF_LOG_ERROR("Error writing ctrl variable");
				return -ENOENT;
			}
		}
	}

	return len;
}

static int ctrl_release(const char *fname,
			struct fuse_file_info *finfo)
{
	struct ctrl_node *node;
	int rc;

	if (!ctrl_fs.started)
		return 0;

	IOF_LOG_INFO("ctrl fs release called for %s", fname);

	rc = find_path_node(fname, &node);

	if (rc != 0 || node == NULL || node->initialized == 0)
		return -ENOENT;

	if (node->ctrl_type == CTRL_COUNTER) {
		int count = (int)finfo->fh;
		ctrl_fs_close_cb_t close_cb;
		void *cb_arg = GET_DATA(node, cnt, cb_arg);

		close_cb = GET_DATA(node, cnt, close_cb);

		if (close_cb != NULL) {
			rc = close_cb(count, cb_arg);
			if (rc != 0) {
				IOF_LOG_ERROR("Error closing ctrl counter");
				return -ENOENT;
			}
		}
	}

	return 0;
}

static int ctrl_utimens(const char *fname, const struct timespec tv[2])
{
	struct ctrl_node *node;
	int rc;

	if (!ctrl_fs.started)
		return 0;

	IOF_LOG_INFO("ctrl fs utimens called for %s", fname);

	rc = find_path_node(fname, &node);

	if (rc != 0 || node == NULL || node->initialized == 0)
		return -ENOENT;

	if (node->ctrl_type == CTRL_EVENT) {
		ctrl_fs_trigger_cb_t trigger_cb;
		void *cb_arg = GET_DATA(node, evnt, cb_arg);

		trigger_cb = GET_DATA(node, evnt, trigger_cb);
		if (trigger_cb != NULL) {
			trigger_cb(cb_arg);
			if (rc != 0) {
				IOF_LOG_ERROR("Error triggering ctrl variable");
				return -ENOENT;
			}
		}
	}

	return 0;
}

static struct fuse_operations fuse_ops = {
	.getattr = ctrl_getattr,
	.open = ctrl_open,
	.read = ctrl_read,
	.truncate = ctrl_truncate,
	.write = ctrl_write,
	.release = ctrl_release,
	.mknod = ctrl_mknod,
	.readdir = ctrl_readdir,
	.utimens = ctrl_utimens,
};


int ctrl_fs_start(const char *prefix)
{
	struct fuse_args args;
	struct stat stat_info;
	int rc;
	char *dash_d = "-d";

	pthread_once(&once_init, init_root_node);

	if (ctrl_fs.startup_rc != 0)
		return ctrl_fs.startup_rc;

	rc = mkdir(prefix, 0700);

	if (rc != 0 && errno != EEXIST) {
		ctrl_fs.startup_rc = -errno;
		IOF_LOG_ERROR("Could not create %s for ctrl fs: %s",
			      prefix, strerror(errno));
		return ctrl_fs.startup_rc;
	}

	if (rc == EEXIST) {
		/* Make sure it's a directory */
		rc = stat(prefix, &stat_info);
		if (rc != 0 || !S_ISDIR(stat_info.st_mode)) {
			IOF_LOG_ERROR("Could not create %s for ctrl fs: %s",
				      prefix, strerror(errno));
			ctrl_fs.startup_rc = -EEXIST;
			return -EEXIST;
		}
	}

	ctrl_fs.prefix = strdup(prefix);

	if (ctrl_fs.prefix == NULL) {
		IOF_LOG_ERROR("Could not allocate memory for ctrl fs");
		ctrl_fs.startup_rc = -ENOMEM;
		return -ENOMEM;
	}

	args.argc = 1;
	args.argv = &dash_d;
	args.allocated = 0;

#ifdef IOF_USE_FUSE3
	ctrl_fs.fuse = fuse_new(&args, &fuse_ops, sizeof(fuse_ops),
				NULL);
	if (ctrl_fs.fuse == NULL) {
		IOF_LOG_ERROR("Could not initialize ctrl fs");
		ctrl_fs.startup_rc = -EIO;
		goto out;
	}

	rc = fuse_mount(ctrl_fs.fuse, ctrl_fs.prefix);
	if (rc == -1) {
		IOF_LOG_ERROR("Could not mount ctrl fs");
		ctrl_fs.startup_rc = -EIO;
		goto out;
	}
#else
	ctrl_fs.ch = fuse_mount(ctrl_fs.prefix, &args);
	if (ctrl_fs.ch == NULL) {
		IOF_LOG_ERROR("Could not mount ctrl fs");
		ctrl_fs.startup_rc = -EIO;
		goto out;
	}

	ctrl_fs.fuse = fuse_new(ctrl_fs.ch, &args, &fuse_ops, sizeof(fuse_ops),
				NULL);
	if (ctrl_fs.fuse == NULL) {
		IOF_LOG_ERROR("Could not initialize ctrl fs");
		ctrl_fs.startup_rc = -EIO;
		goto out;
	}
#endif
	fuse_opt_free_args(&args);

	rc = pthread_create(&ctrl_fs.thread, NULL,
			    ctrl_thread_func, NULL);

	if (rc != 0) {
		IOF_LOG_ERROR("Couldn't start thread for ctrl fs (rc = %d", rc);
		ctrl_fs.startup_rc = -errno;
		goto out;
	}

out:
	if (ctrl_fs.startup_rc != 0)
		cleanup_ctrl_fs();

	if (ctrl_fs.startup_rc == 0)
		ctrl_fs.started = true;

	return ctrl_fs.startup_rc;
}

int ctrl_fs_stop(void)
{
	if (ctrl_fs.startup_rc != 0)
		return 0; /* Assume the error has already been reported */

	ctrl_fs.started = false;

	fuse_exit(ctrl_fs.fuse);

	return 0;
}

int ctrl_fs_wait(void)
{
	int rc;

	if (ctrl_fs.startup_rc != 0)
		return 0; /* Assume the error has already been reported */

	rc = pthread_join(ctrl_fs.thread, NULL);

	if (rc != 0) {
		rc = errno;
		IOF_LOG_ERROR("Error joining ctrl_fs thread %d", rc);
		return -rc;
	}

	cleanup_ctrl_fs();
	cleanup_node(&ctrl_fs.root);

	return 0;
}