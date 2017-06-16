/* Copyright (C) 2016-2017 Intel Corporation
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

#define DEF_LOG_HANDLE ctrl_log_handle
#include "log.h"
#include "ctrl_fs.h"
#include "ctrl_fs_util.h"

static int ctrl_log_handle;

enum {
	CTRL_DIR = 0,
	CTRL_VARIABLE,
	CTRL_EVENT,
	CTRL_CONSTANT,
	CTRL_TRACKER,
	NUM_CTRL_TYPES,
};

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
	char buf[CTRL_FS_MAX_CONSTANT_LEN];
};

struct ctrl_tracker {
	void *cb_arg;
	ctrl_fs_open_cb_t open_cb;
	ctrl_fs_close_cb_t close_cb;
	ctrl_fs_destroy_cb_t destroy_cb;
};

union ctrl_data {
	struct ctrl_variable var;
	struct ctrl_event    evnt;
	struct ctrl_constant con;
	struct ctrl_tracker  tckr;
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

/* Handle created when a file is opened.  A pointer to this is saved
 * in finfo->fh to avoid lookups on further file access
 */
struct open_handle {
	struct ctrl_node *node;
	int st_size;
	int value;
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
#if !IOF_USE_FUSE3
	struct fuse_chan *ch;
#endif
	int next_inode;
	int startup_rc;
	pthread_t thread;
	struct ctrl_node root;
	bool started;
};

struct value_data {
	ctrl_fs_uint64_read_cb_t read;
	ctrl_fs_uint64_write_cb_t write;
	void *arg;
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

	iof_log_init("CTRL", "CTRLFS", &ctrl_log_handle);

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
	size_t size = CTRL_FS_MAX_LEN;
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
	case CTRL_TRACKER:
		dsize = sizeof(struct ctrl_tracker);
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

static int free_child_nodes(struct ctrl_node *node)
{
	struct ctrl_node *item;
	int bad_rc;
	int rc = 0;

	while (!TAILQ_EMPTY(&node->queue)) {
		item = TAILQ_FIRST(&node->queue);
		TAILQ_REMOVE(&node->queue, item, entry);
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

static int cleanup_node(struct ctrl_node *node)
{
	void *cb_arg;
	ctrl_fs_destroy_cb_t destroy_cb = NULL;
	int rc = 0;

	rc = pthread_rwlock_destroy(&node->lock);

	if (rc != 0) {
		IOF_LOG_ERROR("Could not destroy rwlock in ctrl node");
		return rc;
	}

	if (node->ctrl_type == CTRL_DIR)
		return free_child_nodes(node);

	if (node->ctrl_type == CTRL_VARIABLE) {
		cb_arg = GET_DATA(node, var, cb_arg);
		destroy_cb = GET_DATA(node, var, destroy_cb);
	} else if (node->ctrl_type == CTRL_EVENT) {
		cb_arg = GET_DATA(node, evnt, cb_arg);
		destroy_cb = GET_DATA(node, evnt, destroy_cb);
	} else if (node->ctrl_type == CTRL_TRACKER) {
		cb_arg = GET_DATA(node, tckr, cb_arg);
		destroy_cb = GET_DATA(node, tckr, destroy_cb);
	}

	if (destroy_cb == NULL)
		return 0;

	rc = destroy_cb(cb_arg);
	if (rc != 0)
		IOF_LOG_ERROR("Error destroying ctrl node %s", node->name);

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
		IOF_LOG_ERROR("Conflict trying to add %s to ctrl_fs", name);
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

int ctrl_create_subdir(struct ctrl_dir *parent, const char *subdir,
		       struct ctrl_dir **newdir)
{
	struct ctrl_node *node;
	int rc = 0;

	pthread_once(&once_init, init_root_node);

	if (ctrl_fs.startup_rc != 0)
		return ctrl_fs.startup_rc;

	if (newdir == NULL) {
		IOF_LOG_ERROR("Invalid newdir pointer specified");
		return -EINVAL;
	}

	*newdir = NULL;

	if (subdir == NULL) {
		IOF_LOG_ERROR("Invalid subdir specified");
		return -EINVAL;
	}

	if (strchr(subdir, '/') != NULL) {
		IOF_LOG_ERROR("/ not allowed in ctrl subdir '%s'", subdir);
		return -EINVAL;
	}

	if (parent == NULL)
		parent = (struct ctrl_dir *)&ctrl_fs.root;

	node = (struct ctrl_node *)parent;

	rc = add_ctrl_dir(subdir, &node);

	if (rc != 0)
		IOF_LOG_ERROR("Bad subdir %s specified", subdir);

	*newdir = (struct ctrl_dir *)node;
	IOF_LOG_INFO("Registered %s as ctrl subdir", subdir);

	return rc;
}

int ctrl_register_variable(struct ctrl_dir *dir, const char *name,
			   ctrl_fs_read_cb_t read_cb,
			   ctrl_fs_write_cb_t write_cb,
			   ctrl_fs_destroy_cb_t destroy_cb, void *cb_arg)
{
	struct ctrl_node *node;
	int rc = 0;
	int mode = S_IFREG;

	pthread_once(&once_init, init_root_node);

	if (ctrl_fs.startup_rc != 0)
		return ctrl_fs.startup_rc;

	if (name == NULL) {
		IOF_LOG_ERROR("Invalid name specified for ctrl variable");
		return -EINVAL;
	}

	if (strchr(name, '/') != NULL) {
		IOF_LOG_ERROR("/ not allowed in ctrl name '%s'", name);
		return -EINVAL;
	}

	if (dir == NULL)
		dir = (struct ctrl_dir *)&ctrl_fs.root;

	node = (struct ctrl_node *)dir;

	if (read_cb != NULL)
		mode |= S_IRUSR;
	if (write_cb != NULL)
		mode |= S_IWUSR;

	rc = add_ctrl_file(name, &node, mode, CTRL_VARIABLE);

	if (rc != 0)
		IOF_LOG_ERROR("Bad file %s specified", name);

	SET_DATA(node, var, cb_arg, cb_arg);
	SET_DATA(node, var, read_cb, read_cb);
	SET_DATA(node, var, destroy_cb, destroy_cb);
	SET_DATA(node, var, write_cb, write_cb);

	__sync_synchronize();
	node->initialized = 1;

	IOF_LOG_INFO("Registered %s as ctrl variable", name);

	return rc;
}

int ctrl_register_event(struct ctrl_dir *dir, const char *name,
			ctrl_fs_trigger_cb_t trigger_cb,
			ctrl_fs_destroy_cb_t destroy_cb, void *cb_arg)
{
	struct ctrl_node *node;
	int rc = 0;
	int mode = S_IFREG | S_IWUSR;

	pthread_once(&once_init, init_root_node);

	if (ctrl_fs.startup_rc != 0)
		return ctrl_fs.startup_rc;

	if (name == NULL) {
		IOF_LOG_ERROR("Invalid name specified for ctrl variable");
		return -EINVAL;
	}

	if (strchr(name, '/') != NULL) {
		IOF_LOG_ERROR("/ not allowed in ctrl name '%s'", name);
		return -EINVAL;
	}

	if (dir == NULL)
		dir = (struct ctrl_dir *)&ctrl_fs.root;

	node = (struct ctrl_node *)dir;

	rc = add_ctrl_file(name, &node, mode, CTRL_EVENT);

	if (rc != 0)
		IOF_LOG_ERROR("Bad file %s specified", name);

	SET_DATA(node, evnt, cb_arg, cb_arg);
	SET_DATA(node, evnt, trigger_cb, trigger_cb);
	SET_DATA(node, evnt, destroy_cb, destroy_cb);

	__sync_synchronize();
	node->initialized = 1;

	IOF_LOG_INFO("Registered %s as ctrl event", name);
	return rc;
}

int ctrl_register_constant(struct ctrl_dir *dir, const char *name,
			   const char *value)
{
	struct ctrl_node *node;
	int rc = 0;
	int len;

	pthread_once(&once_init, init_root_node);

	if (ctrl_fs.startup_rc != 0)
		return ctrl_fs.startup_rc;

	if (value == NULL) {
		IOF_LOG_ERROR("Invalid value specified for ctrl constant");
		return -EINVAL;
	}

	if (name == NULL) {
		IOF_LOG_ERROR("Invalid name specified for ctrl variable");
		return -EINVAL;
	}

	if (strchr(name, '/') != NULL) {
		IOF_LOG_ERROR("/ not allowed in ctrl name '%s'", name);
		return -EINVAL;
	}

	/* Length handling.  Find the length of the string including the
	 * terminating NULL character
	 */
	len = strlen(value) + 1;
	if (len >= CTRL_FS_MAX_CONSTANT_LEN) {
		IOF_LOG_ERROR("value too long for ctrl constant");
		return -EINVAL;
	}

	if (dir == NULL)
		dir = (struct ctrl_dir *)&ctrl_fs.root;

	node = (struct ctrl_node *)dir;

	rc = add_ctrl_file(name, &node, S_IFREG | S_IRUSR, CTRL_CONSTANT);

	if (rc != 0)
		IOF_LOG_ERROR("Bad file %s specified", name);

	memcpy(GET_DATA(node, con, buf), value, len);

	node->stat_info.st_size = len;

	__sync_synchronize();
	node->initialized = 1;

	IOF_LOG_INFO("Registered %s as ctrl constant.  Value is %s (%d)",
		     name, value, len);
	return rc;
}

#define MAX_INT_STR 32

int ctrl_register_constant_int64(struct ctrl_dir *dir, const char *name,
				 int64_t value)
{
	char buf[MAX_INT_STR];

	snprintf(buf, MAX_INT_STR, "%" PRId64, value);

	return ctrl_register_constant(dir, name, buf);
}

int ctrl_register_constant_uint64(struct ctrl_dir *dir, const char *name,
				  uint64_t value)
{
	char buf[MAX_INT_STR];

	snprintf(buf, MAX_INT_STR, "%" PRIu64, value);

	return ctrl_register_constant(dir, name, buf);
}

int ctrl_register_tracker(struct ctrl_dir *dir, const char *name,
			  ctrl_fs_open_cb_t open_cb,
			  ctrl_fs_close_cb_t close_cb,
			  ctrl_fs_destroy_cb_t destroy_cb,
			  void *cb_arg)
{
	struct ctrl_node *node;
	int rc = 0;

	pthread_once(&once_init, init_root_node);

	if (ctrl_fs.startup_rc != 0)
		return ctrl_fs.startup_rc;

	if (name == NULL) {
		IOF_LOG_ERROR("Invalid name specified for ctrl variable");
		return -EINVAL;
	}

	if (strchr(name, '/') != NULL) {
		IOF_LOG_ERROR("/ not allowed in ctrl name '%s'", name);
		return -EINVAL;
	}

	if (dir == NULL)
		dir = (struct ctrl_dir *)&ctrl_fs.root;

	node = (struct ctrl_node *)dir;

	rc = add_ctrl_file(name, &node, S_IFREG | S_IRUSR, CTRL_TRACKER);

	if (rc != 0)
		IOF_LOG_ERROR("Bad ctrl file %s", name);

	SET_DATA(node, tckr, cb_arg, cb_arg);
	SET_DATA(node, tckr, open_cb, open_cb);
	SET_DATA(node, tckr, close_cb, close_cb);
	SET_DATA(node, tckr, destroy_cb, destroy_cb);

	__sync_synchronize();
	node->initialized = 1;

	IOF_LOG_INFO("Registered %s as ctrl tracker", name);
	return rc;
}

static int ctrl_uint64_read(char *buf, size_t buflen, void *arg)
{
	struct value_data *data = (struct value_data *)arg;
	uint64_t value = data->read(data->arg);

	snprintf(buf, buflen, "%lu", value);
	return 0;
}

static int ctrl_uint64_write(const char *str, void *arg)
{
	struct value_data *data = (struct value_data *)arg;
	uint64_t value = 0;
	int rc;

	rc = sscanf(str, "%lu", &value);
	if (rc != 1)
		return EINVAL;

	rc = data->write(value, data->arg);

	if (rc)
		return rc;

	return 0;
}

static int ctrl_uint64_destroy(void *arg)
{
	free(arg);
	return 0;
}

int ctrl_register_uint64_variable(struct ctrl_dir *dir,
				  const char *name,
				  ctrl_fs_uint64_read_cb_t read_cb,
				  ctrl_fs_uint64_write_cb_t write_cb,
				  void *cb_arg)
{
	struct value_data *data = calloc(1, sizeof(*data));
	int rc;

	if (!data)
		return -ENOMEM;

	data->read = read_cb;
	data->write = write_cb;
	data->arg = cb_arg;

	rc = ctrl_register_variable(dir, name,
				    ctrl_uint64_read,
				    write_cb ? ctrl_uint64_write : NULL,
				    ctrl_uint64_destroy,
				    data);
	if (rc)
		free(data);
	return rc;
}

static void *ctrl_thread_func(void *arg)
{
	int rc;

	IOF_LOG_INFO("Starting ctrl fs loop");

	rc = fuse_loop(ctrl_fs.fuse); /* Blocking */

	IOF_LOG_INFO("Exited ctrl fs loop %d", rc);

	if (rc)
		IOF_LOG_ERROR("Fuse loop exited with %d", rc);

#ifdef IOF_USE_FUSE3
	fuse_unmount(ctrl_fs.fuse);
#else
	if (ctrl_fs.ch)
		fuse_unmount(ctrl_fs.prefix, ctrl_fs.ch);
#endif
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

int ctrl_opendir(const char *path, struct fuse_file_info *finfo)
{
	struct open_handle *handle;
	struct ctrl_node *node;
	int rc;

	if (!ctrl_fs.started)
		return -ENOENT;

	IOF_LOG_INFO("ctrl_fs opendir called for %s", path);

	rc = find_path_node(path, &node);

	if (rc != 0 || node == NULL)
		return -ENOENT;

	if (node->ctrl_type != CTRL_DIR)
		return -ENOTDIR;

	handle = calloc(1, sizeof(*handle));
	if (!handle)
		return -ENOMEM;

	handle->node = node;
	finfo->fh = (uint64_t)handle;

	return 0;
}

#ifdef IOF_USE_FUSE3
static int ctrl_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			off_t offset, struct fuse_file_info *finfo,
			enum fuse_readdir_flags flags)
#else
static int ctrl_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			off_t offset, struct fuse_file_info *finfo)
#endif
{
	struct open_handle *handle = (struct open_handle *)finfo->fh;
	struct ctrl_node *node = handle->node;
	struct ctrl_node *item;

	IOF_LOG_INFO("ctrl_fs readdir called for %s", node->name);

	pthread_rwlock_rdlock(&node->lock);
	/* There doesn't seem to be an appropriate readdir error code if this
	 * fails.   So I guess let it race, I guess.
	 */

	TAILQ_FOREACH(item, &node->queue, entry) {
		if (item->initialized == 0)
			continue;

		if (filler(buf, item->name, &item->stat_info, 0
#ifdef IOF_USE_FUSE3
			   , 0
#endif
			   ))
			break;
	}

	pthread_rwlock_unlock(&node->lock);

	return 0;
}

int ctrl_releasedir(const char *dir, struct fuse_file_info *finfo)
{
	struct open_handle *handle = (struct open_handle *)finfo->fh;

	free(handle);
	return 0;
}

static int ctrl_getattr(const char *fname, struct stat *stat)
{
	struct ctrl_node *node = NULL;
	int rc;

	IOF_LOG_INFO("ctrl_fs getattr called for %s", fname);

	rc = find_path_node(fname, &node);

	if (rc != 0 || !node) {
		IOF_LOG_INFO("Failed for %s %d", fname, rc);
		return -ENOENT;
	}

	IOF_LOG_INFO("Returning getattr for '%s' mode = 0%o", node->name,
		     node->stat_info.st_mode & ~(S_IFMT));
	memcpy(stat, &node->stat_info, sizeof(struct stat));

	return 0;
}

#ifdef IOF_USE_FUSE3
static int ctrl_getattr3(const char *fname, struct stat *stat,
			 struct fuse_file_info *finfo)
{
	struct open_handle *handle;
	struct ctrl_node *node;

	if (!finfo)
		return ctrl_getattr(fname, stat);

	handle = (struct open_handle *)finfo->fh;
	node = handle->node;

	IOF_LOG_INFO("Returning getfattr for '%s' mode = 0%o", node->name,
		     node->stat_info.st_mode & ~(S_IFMT));
	memcpy(stat, &node->stat_info, sizeof(struct stat));
	if (handle->st_size != 0)
		stat->st_size = handle->st_size;

	return 0;

}
#endif

static int ctrl_open(const char *fname, struct fuse_file_info *finfo)
{
	struct ctrl_node *node;
	struct open_handle *handle;
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

	handle = calloc(1, sizeof(*handle));
	if (!handle)
		return -ENOMEM;

	handle->node = node;
	finfo->fh = (uint64_t)handle;

	if (node->ctrl_type == CTRL_TRACKER) {
		int value = 0;
		ctrl_fs_open_cb_t open_cb;
		void *cb_arg = GET_DATA(node, tckr, cb_arg);

		open_cb = GET_DATA(node, tckr, open_cb);

		if (open_cb != NULL)
			open_cb(&value, cb_arg);

		handle->value = value;
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

#if IOF_USE_FUSE3
static int ctrl_truncate3(const char *fname, off_t size,
			  struct fuse_file_info *fi)
{
	if (fi)
		return 0;

	return ctrl_truncate(fname, size);
}
#endif

static int ctrl_read(const char *fname,
		     char *buf,
		     size_t size,
		     off_t offset,
		     struct fuse_file_info *finfo)
{
	struct open_handle *handle = (struct open_handle *)finfo->fh;
	struct ctrl_node *node = handle->node;
	char mybuf[CTRL_FS_MAX_LEN];
	const char *payload;
	size_t len;
	int rc;

	IOF_LOG_INFO("ctrl fs read called for %s", node->name);

	if (offset != 0) {
		IOF_LOG_WARNING("Invalid offset %ld for %s\n", offset,
				node->name);
		return -EINVAL;
	}

	if (node->ctrl_type == CTRL_CONSTANT)
		payload = GET_DATA(node, con, buf);
	else if (node->ctrl_type == CTRL_VARIABLE) {
		ctrl_fs_read_cb_t read_cb;
		void *cb_arg = GET_DATA(node, var, cb_arg);

		read_cb = GET_DATA(node, var, read_cb);

		if (!read_cb) {
			IOF_LOG_ERROR("No callback reading ctrl variable");
			return -EIO;
		}
		rc = read_cb(mybuf, CTRL_FS_MAX_LEN, cb_arg);
		if (rc != 0) {
			IOF_LOG_ERROR("Error reading ctrl variable");
			return -ENOENT;
		}
		payload = mybuf;
	} else if (node->ctrl_type == CTRL_TRACKER) {
		sprintf(mybuf, "%d", handle->value);
		payload = mybuf;
	} else {
		IOF_LOG_WARNING("Read not supported for ctrl node %s",
				node->name);
		return -EINVAL;
	}

	len = snprintf(buf, size, "%s\n", payload);
	if (len >= size) {
		len = size;
		IOF_LOG_WARNING("Truncated value for %s", node->name);
		buf[size - 1] = '\n';
	}

	IOF_LOG_INFO("Done copying contents to output buffer %s %zi len is %ld",
		     node->name, size, len);

	if (len > 0) {
		node->stat_info.st_size = len;
		handle->st_size = len;
	}

	return len;
}

static int ctrl_write(const char *fname,
		      const char *buf,
		      size_t len,
		      off_t offset,
		      struct fuse_file_info *finfo)
{
	struct open_handle *handle = (struct open_handle *)finfo->fh;
	struct ctrl_node *node = handle->node;
	char mybuf[CTRL_FS_MAX_LEN];
	int rc;

	IOF_LOG_INFO("ctrl fs write called for %s", node->name);

	if (offset != 0) {
		IOF_LOG_WARNING("Invalid offset %ld for %s\n", offset,
				node->name);
		return -EINVAL;
	}

	if (node->ctrl_type == CTRL_EVENT) {
		ctrl_fs_trigger_cb_t trigger_cb;
		void *cb_arg = GET_DATA(node, evnt, cb_arg);

		trigger_cb = GET_DATA(node, evnt, trigger_cb);

		if (trigger_cb != NULL) {
			rc = trigger_cb(cb_arg);
			if (rc != 0) {
				IOF_LOG_ERROR("Error triggering ctrl event");
				return -ENOENT;
			}
		}
	} else if (node->ctrl_type == CTRL_VARIABLE) {
		ctrl_fs_write_cb_t write_cb;
		void *cb_arg = GET_DATA(node, var, cb_arg);

		write_cb = GET_DATA(node, var, write_cb);

		if (write_cb != NULL) {
			size_t mylen = len;

			if (len > (CTRL_FS_MAX_LEN - 1))
				mylen = CTRL_FS_MAX_LEN - 1;

			memcpy(mybuf, buf, mylen);
			mybuf[mylen] = 0;
			rc = write_cb(mybuf, cb_arg);
			if (rc != 0) {
				IOF_LOG_ERROR("Error writing ctrl variable");
				return -rc;
			}
		}
	}

	if (len > 0) {
		node->stat_info.st_size = len;
		handle->st_size = len;
	}

	return len;
}

static int ctrl_release(const char *fname,
			struct fuse_file_info *finfo)
{
	struct open_handle *handle = (struct open_handle *)finfo->fh;
	struct ctrl_node *node = handle->node;
	int rc;

	IOF_LOG_INFO("ctrl fs release called for %s", node->name);

	if (node->ctrl_type == CTRL_TRACKER) {
		ctrl_fs_close_cb_t close_cb;
		void *cb_arg = GET_DATA(node, tckr, cb_arg);

		close_cb = GET_DATA(node, tckr, close_cb);

		if (close_cb != NULL) {
			rc = close_cb(handle->value, cb_arg);
			if (rc != 0) {
				IOF_LOG_ERROR("Error closing ctrl tracker");
				return -ENOENT;
			}
		}
	}

	IOF_LOG_INFO("releasing memory %p", handle);
	free(handle);

	return 0;
}

#if IOF_USE_FUSE3
static void *ctrl_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	struct fuse_context *context = fuse_get_context();
	void *handle = (void *)context->private_data;

	IOF_LOG_INFO("Fuse configuration for ctrl fs");

	cfg->entry_timeout = 0;
	cfg->negative_timeout = 0;
	cfg->attr_timeout = 0;
	cfg->remember = -1;
	cfg->nullpath_ok = 1;

	IOF_LOG_INFO("timeouts entry %f negative %f attr %f",
		     cfg->entry_timeout, cfg->negative_timeout,
		     cfg->attr_timeout);

	return handle;
}
#endif

static struct fuse_operations fuse_ops = {
#if IOF_USE_FUSE3
	.init = ctrl_init,
	.getattr = ctrl_getattr3,
	.truncate = ctrl_truncate3,
#else
	.getattr = ctrl_getattr,
	.truncate = ctrl_truncate,
#endif
	.open = ctrl_open,
	.read = ctrl_read,
	.write = ctrl_write,
	.release = ctrl_release,
	.opendir = ctrl_opendir,
	.readdir = ctrl_readdir,
	.releasedir = ctrl_releasedir,
};


int ctrl_fs_start(const char *prefix)
{
	static char const *opts[] = {"", "-ofsname=CNSS",
				     "-osubtype=ctrl"};
	struct fuse_args args = {0};

	struct stat stat_info;
	int rc;

	args.argc = sizeof(opts) / sizeof(*opts);
	args.argv = (char **)opts;
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
		IOF_LOG_ERROR("Couldn't start thread for ctrl fs (rc = %d)",
			      rc);
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
	iof_log_close();

	return 0;
}
