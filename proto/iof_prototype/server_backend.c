#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libgen.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include "server_backend.h"

struct fs_info info;

static void _ent_init(struct fs_ent *ent, const char *name, mode_t mode)
{
	time_t seconds = time(NULL);

	LOCK_INIT(&ent->lock);

	ent->stat.st_ctime = seconds;
	ent->stat.st_atime = seconds;
	ent->stat.st_mtime = seconds;

	strncpy(ent->name, name, MAX_NAME_LEN);
	ent->stat.st_mode = mode;
	ent->stat.st_nlink = 1;
	ent->stat.st_uid = getuid();
	ent->stat.st_gid = getgid();
}

int filesystem_init(void)
{
	struct fs_desc *fd = new_fs_desc();

	info.root = &fd->top_dir;
	return 0;
}

struct fs_desc *new_fs_desc()
{
	struct fs_desc *desc = malloc(sizeof(struct fs_desc));

	if (!desc)
		return NULL;

	memset(desc, 0, sizeof(*desc));
	_ent_init(&desc->top_dir.ent, "", S_IFDIR | 0700);

	desc->top_dir.ent.stat.st_nlink = 2;

	desc->inode_count = (1024 * 64);
	LOCK_INIT(&desc->alloc_lock);
	return desc;
}

static off_t next_delim(const char *name)
{
	int index = 0;

	while (name[index] != '/' && name[index] != '\0')
		index++;
	return index;
}

struct fs_dir *find_p_dir(const char *name, char **basename,
			  struct fs_dir *top_dir, int write)
{
	struct fs_dir *parent_dir;
	off_t dir_len;

	if (strncmp(name, "/", 2) == 0) {
		if (basename)
			*basename = NULL;
		if (write) {
			printf("locking root in write mode\n");
			WRITE_LOCK(&top_dir->ent.lock);
		} else {
			READ_LOCK(&top_dir->ent.lock);
		}
		return top_dir;
	}
	parent_dir = top_dir;

	dir_len = next_delim(&name[1]);

	if (write && name[dir_len + 1] == '\0') {
		printf("locking / in write mode\n");
		WRITE_LOCK(&top_dir->ent.lock);
	} else {
		READ_LOCK(&top_dir->ent.lock);
	}

	while (name[dir_len + 1] != '\0') {
		off_t next_dir_len;
		struct fs_dir *child_dir = parent_dir->child_dirs;

		name++;

		while (child_dir) {
			if ((strncmp(child_dir->ent.name, name, dir_len) == 0)
				&& (child_dir->ent.name[dir_len] == '\0'))
				break;

			child_dir = (struct fs_dir *)child_dir->ent.ent_next;
		}

		if (!child_dir) {
			if (basename)
				*basename = NULL;
			READ_UNLOCK(&parent_dir->ent.lock);
			return NULL;
		}
		next_dir_len = next_delim(&name[dir_len + 1]);

		if (write && name[dir_len + next_dir_len + 1] == '\0')
			WRITE_LOCK(&child_dir->ent.lock);
		else
			READ_LOCK(&child_dir->ent.lock);

		READ_UNLOCK(&parent_dir->ent.lock);
		parent_dir = child_dir;
		name += dir_len;

		dir_len = next_dir_len;
	}

	if (basename)
		*basename = (char *)&name[1];
	return parent_dir;
}

int iof_getattr(const char *name, struct stat *stbuf)
{
	char *base;
	struct fs_dir *dir;
	struct fs_ent *ent;

	if (strcmp(name, "/") == 0) {
		memcpy(stbuf, &info.root->ent.stat, sizeof(struct stat));
		return 0;
	}

	dir = find_p_dir(name, &base, info.root, 0);

	if (!dir)
		return -1;

	ent = &dir->first_file->ent;

	while (ent) {
		if (strncmp(ent->name, base, MAX_NAME_LEN) == 0) {
			memcpy(stbuf, &ent->stat, sizeof(struct stat));
			READ_UNLOCK(&dir->ent.lock);
			return 0;
		}
		ent = ent->ent_next;
	}

	ent = &dir->child_dirs->ent;

	while (ent) {
		if (strncmp(ent->name, base, MAX_NAME_LEN) == 0) {
			memcpy(stbuf, &ent->stat, sizeof(struct stat));
			READ_UNLOCK(&dir->ent.lock);
			return 0;
		}
		ent = ent->ent_next;
	}
	ent = &dir->first_link->ent;
	while (ent) {
		if (strncmp(ent->name, base, MAX_NAME_LEN) == 0) {
			memcpy(stbuf, &ent->stat, sizeof(struct stat));
			READ_UNLOCK(&dir->ent.lock);
			return 0;
		}
		ent = ent->ent_next;
	}

	READ_UNLOCK(&dir->ent.lock);
	return -ENOENT;
}

/* Actual readdir implementation*/
uint64_t
iof_readdir(char *name, const char *dir_name, uint64_t *error,
	    uint64_t in_offset, struct stat *stat)
{
	struct fs_dir *dir = find_dir(dir_name, info.root, 0);
	struct fs_ent *ent;
	int i;

	if (!dir) {
		*error = -ENOENT;
		return 0;
	}
	ent = &dir->child_dirs->ent;
	for (i = 0; ent; i++) {
		if (i == in_offset) {
			strncpy(name, ent->name, sizeof(ent->name));
			memcpy(stat, &ent->stat, sizeof(*stat));
			READ_UNLOCK(&dir->ent.lock);
			*error = 0;
			if (ent->ent_next)
				return 0;
			return (dir->first_link == 0);
		}
		ent = ent->ent_next;
	}
	ent = &dir->first_link->ent;
	for (; ent; i++) {
		if (i == in_offset) {
			strncpy(name, ent->name, sizeof(ent->name));
			memcpy(stat, &ent->stat, sizeof(*stat));
			READ_UNLOCK(&dir->ent.lock);
			*error = 0;
			return (ent->ent_next == 0);
		}
		ent = ent->ent_next;
	}
	READ_UNLOCK(&dir->ent.lock);
	*error = -ENOENT;
	return 1;
}

uint64_t iof_mkdir(const char *name, mode_t mode)
{
	char *base;
	struct fs_dir *p_dir = find_p_dir(name, &base, info.root, 1);
	struct fs_dir *dir;

	if (!p_dir)
		return -ENOENT;

	if (is_name_in_use(base, p_dir) != 0) {
		READ_UNLOCK(&p_dir->ent.lock);
		return -EEXIST;
	}
	dir = new_dir(base, mode);
	dir->ent.ent_next = &p_dir->child_dirs->ent;
	p_dir->child_dirs = dir;
	if (dir->ent.ent_next)
		dir->ent.ent_next->ent_prev = &dir->ent;

	READ_UNLOCK(&p_dir->ent.lock);
	return 0;
}

uint64_t iof_rmdir(const char *name)
{
	char *base;
	struct fs_dir *p_dir = find_p_dir(name, &base, info.root, 1);
	struct fs_dir *dir = p_dir->child_dirs;

	while (dir) {
		if (strncmp(dir->ent.name, base, MAX_NAME_LEN) == 0) {
			WRITE_LOCK(&dir->ent.lock);
			if (!dir->ent.ent_prev) {
				p_dir->child_dirs = (struct fs_dir *)dir->ent.ent_next;
				if (dir->ent.ent_next)
					dir->ent.ent_next->ent_prev = NULL;
				WRITE_UNLOCK(&dir->ent.lock);
				LOCK_DESTROY(&dir->ent.lock);
				free(dir);
				WRITE_UNLOCK(&p_dir->ent.lock);
				return 0;
			}
			dir->ent.ent_prev->ent_next = dir->ent.ent_next;
			if (dir->ent.ent_next)
				dir->ent.ent_next->ent_prev = dir->ent.ent_prev;
			WRITE_UNLOCK(&dir->ent.lock);
			LOCK_DESTROY(&dir->ent.lock);
			free(dir);
			WRITE_UNLOCK(&p_dir->ent.lock);
			return 0;
		}
		dir = (struct fs_dir *)dir->ent.ent_next;
	}
	WRITE_UNLOCK(&p_dir->ent.lock);
	return -ENOENT;
}

struct fs_dir *find_dir(const char *name, struct fs_dir *top_dir, int write)
{
	char *base;
	struct fs_dir *parent_dir;
	struct fs_dir *iter_dir;

	if (strncmp(name, "/", 2) == 0) {
		if (write) {
			printf("locking root in write mode\n");
			WRITE_LOCK(&top_dir->ent.lock);
		} else {
			READ_LOCK(&top_dir->ent.lock);
		}

		return top_dir;
	}
	parent_dir = find_p_dir(name, &base, top_dir, 0);

	iter_dir = parent_dir->child_dirs;
	while (iter_dir) {
		if (strncmp(iter_dir->ent.name, base, MAX_NAME_LEN) == 0) {
			if (write)
				WRITE_LOCK(&iter_dir->ent.lock);
			else
				READ_LOCK(&iter_dir->ent.lock);
			READ_UNLOCK(&parent_dir->ent.lock);
			return iter_dir;
		}
		iter_dir = (struct fs_dir *)iter_dir->ent.ent_next;
	}
	READ_UNLOCK(&parent_dir->ent.lock);
	return NULL;
}

uint64_t iof_symlink(const char *dst, const char *name)
{
	char *base;
	struct fs_dir *p_dir = find_p_dir(name, &base, info.root, 1);
	struct fs_link *link;

	if (!p_dir)
		return -ENOENT;

	if (is_name_in_use(base, p_dir)) {
		WRITE_UNLOCK(&p_dir->ent.lock);
		return -EEXIST;
	}
	link = new_link(base);

	if (!link) {
		WRITE_UNLOCK(&p_dir->ent.lock);
		return -ENOMEM;
	}

	link->contents = strndup(dst, MAX_NAME_LEN);

	link->ent.ent_next = &p_dir->first_link->ent;
	if (link->ent.ent_next)
		link->ent.ent_next->ent_prev = &link->ent;
	p_dir->first_link = link;

	WRITE_UNLOCK(&p_dir->ent.lock);
	return 0;
}

uint64_t iof_readlink(const char *name, char **dst)
{
	char *base;
	struct fs_dir *p_dir = find_p_dir(name, &base, info.root, 1);
	struct fs_link *link;

	if (!p_dir)
		return -ENOENT;

	link = find_link_p(base, p_dir);

	if (!link) {
		READ_UNLOCK(&p_dir->ent.lock);
		return -ENOENT;
	}
	READ_LOCK(&link->ent.lock);
	READ_UNLOCK(&p_dir->ent.lock);
	*dst = strndup(link->contents, MAX_NAME_LEN);
	READ_UNLOCK(&link->ent.lock);
	return 0;
}

static void
_backend_unlink_gen(struct fs_dir *dir,
		    struct fs_ent *ent, struct fs_ent **first)
{
	if (!ent->ent_prev) {
		*first = ent->ent_next;
		if (ent->ent_next)
			ent->ent_next->ent_prev = NULL;
		return;
	}
	ent->ent_prev->ent_next = ent->ent_next;
	if (ent->ent_next)
		ent->ent_next->ent_prev = ent->ent_prev;
}

static void
_backend_destroy_file(struct fs_desc *fd,
		      struct fs_dir *dir, struct fs_file *file)
{
	_backend_unlink_gen(dir, &file->ent, (struct fs_ent **)&dir->first_file);
	if (file->contents)
		free(file->contents);
	WRITE_UNLOCK(&file->ent.lock);
	LOCK_DESTROY(&file->ent.lock);
	free(file);
	WRITE_LOCK(&fd->alloc_lock);
	fd->inode_used--;
	WRITE_UNLOCK(&fd->alloc_lock);
}

static void
_backend_destroy_link(struct fs_desc *fd,
		      struct fs_dir *dir, struct fs_link *link)
{
	_backend_unlink_gen(dir, &link->ent, (struct fs_ent **)&dir->first_link);
	if (link->contents)
		free(link->contents);
	WRITE_UNLOCK(&link->ent.lock);
	LOCK_DESTROY(&link->ent.lock);
	free(link);
	WRITE_LOCK(&fd->alloc_lock);
	fd->inode_used--;
	WRITE_UNLOCK(&fd->alloc_lock);
}

uint64_t iof_unlink(const char *name)
{
	struct fs_desc *fd = new_fs_desc();
	char *base;
	struct fs_dir *p_dir = find_p_dir(name, &base, info.root, 1);
	struct fs_ent *ent = &p_dir->first_file->ent;

	while (ent) {
		if (strncmp(ent->name, base, MAX_NAME_LEN) == 0) {
			WRITE_LOCK(&ent->lock);
			_backend_destroy_file(fd, p_dir, (struct fs_file *)ent);
			WRITE_UNLOCK(&p_dir->ent.lock);
			return 0;
		}
		ent = ent->ent_next;
	}

	ent = &p_dir->first_link->ent;
	while (ent) {
		if (strncmp(ent->name, base, MAX_NAME_LEN) == 0) {
			WRITE_LOCK(&ent->lock);
			_backend_destroy_link(fd, p_dir, (struct fs_link *)ent);
			WRITE_UNLOCK(&p_dir->ent.lock);
			return 0;
		}
		ent = ent->ent_next;
	}

	READ_UNLOCK(&p_dir->ent.lock);
	return -ENOENT;
}

int is_name_in_use(const char *basename, struct fs_dir *parent_dir)
{
	struct fs_ent *ent = &parent_dir->first_file->ent;

	while (ent) {
		if (strncmp(basename, ent->name, MAX_NAME_LEN) == 0)
			return 1;
		ent = ent->ent_next;
	}

	ent = &parent_dir->child_dirs->ent;
	while (ent) {
		if (strncmp(basename, ent->name, MAX_NAME_LEN) == 0)
			return 1;
		ent = ent->ent_next;
	}

	ent = &parent_dir->first_link->ent;
	while (ent) {
		if (strncmp(basename, ent->name, MAX_NAME_LEN) == 0)
			return 1;
		ent = ent->ent_next;
	}
	return 0;
}

struct fs_dir *new_dir(const char *name, mode_t mode)
{
	struct fs_dir *dir = malloc(sizeof(struct fs_dir));

	if (!dir)
		return NULL;
	memset(dir, 0, sizeof(*dir));

	_ent_init(&dir->ent, name, S_IFDIR | mode);
	return dir;
}

struct fs_link *new_link(const char *name)
{
	struct fs_link *link = malloc(sizeof(struct fs_link));

	if (!link)
		return NULL;
	memset(link, 0, sizeof(*link));

	_ent_init(&link->ent, name, S_IFLNK | 0755);

	return link;
}

struct fs_link *find_link_p(char *basename, struct fs_dir *dir)
{
	struct fs_ent *ent = &dir->first_link->ent;

	while (ent) {
		if (strncmp(basename, ent->name, MAX_NAME_LEN) == 0)
			return (struct fs_link *)ent;
		ent = ent->ent_next;
	}
	return NULL;
}
