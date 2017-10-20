/* Copyright (C) 2017 Intel Corporation
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
#ifndef __IONSS_H__
#define __IONSS_H__

#include <dirent.h>
#include <sys/mman.h>
#include <stdbool.h>
#include "iof_atomic.h"
#include "ios_gah.h"
#include "iof_pool.h"
#include "iof_bulk.h"

#include <gurt/list.h>
#include <gurt/hash.h>

#define IOF_MAX_FSTYPE_LEN 32

struct ios_base {
	struct ios_projection	*projection_array;
	struct iof_fs_info	*fs_list;
	struct ios_gah_store	*gs;
	struct proto		*proto;
	struct iof_pool		pool;
	uint			projection_count;
	crt_group_t		*primary_group;
	d_rank_t		my_rank;
	uint32_t		num_ranks;
	uint			poll_interval;
	crt_context_t		crt_ctx;
	pthread_rwlock_t	gah_rwlock;
	uint32_t		max_read;
	uint32_t		max_iov_read;
	uint32_t		max_write;
	uint32_t		max_readdir;
};

/* A miniature struct that describes a file handle, this is used
 * in a couple of ways, firsly as the key to the file_handle
 * hash table but also as a small struct which can be created
 * on the stack allowing for hash-table searching before creating
 * a larger ionss_file_handle struct.
 */

enum ionss_fh_type {
	open_handle,
	inode_handle,
};

struct ionss_mini_file {
	int			flags; /* File open flags */
	ino_t			inode_no;
	enum ionss_fh_type	type;
};

/* File descriptor for open file handles.
 *
 * This structure exists from open-to-close for all open files, and is shared
 * across all clients which opened the same file (as tested by inode number)
 * with the same open flags.
 *
 * When handling RPCs the pointer is retrieved from the GAH code using a lookup
 * table, however descriptors are also kept in the file_ht consistent hash
 * table which is checked on open to allow sharing of descriptors across
 * clients.
 *
 * Hash table reference counting is used to keep track of client access, the
 * reference count is simply the number of clients who hold a copy of the GAH.
 * No reference count is held for the hash table entry itself, and the decref()
 * function will remove the descriptor from the hash table when the count
 * decreases to zero.
 *
 * File handle reference counting is performed as well, and this counts one
 * entry for the hash table reference, plus one for every locall thread
 * currently performing operations on the file.
 *
 * The last instance of file close will result in decref to zero in the ht which
 * will then call fh_decref(), which will then release the GAH and recycle the
 * descriptor.
 */
struct ionss_file_handle {
	struct ios_gah		 gah;
	struct ios_projection	*projection;
	d_list_t		 clist;
	struct ionss_mini_file	 mf;
	uint			 fd;
	ATOMIC uint		 ht_ref;
	ATOMIC uint		 ref;
};

struct ios_projection {
	struct ios_base		*base;
	char			*full_path;
	char			fs_type[IOF_MAX_FSTYPE_LEN];
	struct iof_pool_type	*fh_pool;
	struct iof_pool_type	*ar_pool;
	struct ionss_file_handle	*root;
	struct d_chash_table	file_ht;
	uint			id;
	uint			flags;
	uint			active;
	uint64_t		dev_no;
	pthread_mutex_t		lock;
	int			current_read_count;
	int			max_read_count;
	d_list_t		read_list;
};

/* Convert from a fs_id as received over the network to a projection pointer.
 * Returns a possibly NULL pointer to a ios_projection struct.
 */
#define ID_TO_PROJECTION(BASE, ID) ((ID) < (BASE)->projection_count \
					? &(BASE)->projection_array[ID] : NULL)

/* Convert from a fs_id to a directory fd.  Users of this macro should be
 * updated to use ID_TO_PROJECTION instead.
 */
#define ID_TO_FD(ID) (base.projection_array[(ID)].root->fd)

struct ionss_dir_handle {
	DIR	*h_dir;
	uint	fd;
	off_t	offset;
};

#define IONSS_READDIR_ENTRIES_PER_RPC (2)

/*
 * Pipelining reads.
 *
 */

/* Read request descriptor
 *
 * Used to describe a read request.  There is one of these per RPC that the
 * IONSS receives.
 */
struct ionss_read_req_desc {
	crt_rpc_t			*rpc;
	struct ionss_file_handle	*handle;
	struct iof_read_bulk_out	*out;
	struct iof_read_bulk_in         *in;
	size_t				req_len;
	struct ionss_active_read	*ard;
	d_list_t			list;
};

/* Active read descriptor
 *
 * Used to descrive an in-progress read request.  These consume resources so
 * are limited to a fixed number.
 */
struct ionss_active_read {
	struct ios_projection		*projection;
	struct ionss_read_req_desc      *rrd;
	struct iof_local_bulk		local_bulk;
	d_list_t			list;
	size_t				buf_len;
	size_t				read_len;
	bool				failed;

};

/* From fs.c */

/* Create a new fh.
 *
 * This will allocate the structure, add it to the GAH lookup tables, zero up
 * and take a reference.
 */
int ios_fh_alloc(struct ios_projection *, struct ionss_file_handle **);

/* Decrease the reference count on the fh, and if it drops to zero
 * then release it by freeing memory and removing it from the lookup tables.
 *
 * Should be called with a count of 1 for every handle returned by ios_fh_find()
 */
void ios_fh_decref(struct ionss_file_handle *, int);

/* Lookup a file handle from a GAH and take a reference to it.
 */
struct ionss_file_handle *ios_fh_find_real(struct ios_base *,
					   struct ios_gah *,
					   const char *);

struct ionss_dir_handle *ios_dirh_find_real(struct ios_base *,
					    struct ios_gah *,
					    const char *);

#define ios_fh_find(BASE, GAH) ios_fh_find_real((BASE), (GAH), __func__)

#define ios_dirh_find(BASE, GAH) ios_dirh_find_real((BASE), (GAH), __func__)

#endif
