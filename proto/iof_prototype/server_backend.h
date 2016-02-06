
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
