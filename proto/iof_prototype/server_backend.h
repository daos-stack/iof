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

#ifndef SERVER_BACKEND_H
#define SERVER_BACKEND_H

#include<pthread.h>
#include<stdint.h>

#define MAX_NAME_LEN 255
#define RW_LOCK pthread_rwlock_t

#define LOCK_INIT(LOCK) pthread_rwlock_init(LOCK, NULL)

#define LOCK_DESTROY(LOCK) pthread_rwlock_destroy(LOCK)

#define READ_LOCK(LOCK) pthread_rwlock_rdlock(LOCK)
#define WRITE_LOCK(LOCK) pthread_rwlock_wrlock(LOCK)
#define READ_UNLOCK(LOCK) pthread_rwlock_unlock(LOCK)
#define WRITE_UNLOCK(LOCK) pthread_rwlock_unlock(LOCK)

struct fs_ent {
	char name[255];
	struct stat stat;
	RW_LOCK lock;
	struct fs_ent *ent_next;
	struct fs_ent *ent_prev;
};

struct fs_file {
	struct fs_ent ent;
	int open;
	size_t c_size;
	int dirty;
	void *contents;
};

struct fs_dir {
	struct fs_ent ent;
	struct fs_dir *child_dirs;
	struct fs_file *first_file;
	struct fs_link *first_link;

};

struct fs_link {
	struct fs_ent ent;
	void *contents;
};

struct fs_desc {
	struct fs_dir top_dir;
	int inode_count;
	int inode_used;
	RW_LOCK alloc_lock;
};

struct fs_info {
	struct fs_dir *root;
};

int filesystem_init(void);

struct fs_desc *new_fs_desc();

struct fs_dir *find_p_dir(const char *name, char **basename,
			  struct fs_dir *top_dir, int write);
int iof_getattr(const char *name, struct stat *stbuf);
uint64_t iof_readdir(char *name, const char *dir_name, uint64_t *error,
		     uint64_t in_offset, struct stat *stbuf);
uint64_t iof_mkdir(const char *name, mode_t mode);
uint64_t iof_rmdir(const char *name);
uint64_t iof_symlink(const char *dst, const char *name);
uint64_t iof_readlink(const char *name, char **dst);
uint64_t iof_unlink(const char *name);

struct fs_dir *find_dir(const char *name, struct fs_dir *top_dir, int write);
int is_name_in_use(const char *basename, struct fs_dir *parent_dir);
struct fs_dir *new_dir(const char *name, mode_t mode);
struct fs_link *new_link(const char *name);
struct fs_link *find_link_p(char *basename, struct fs_dir *dir);

#endif
