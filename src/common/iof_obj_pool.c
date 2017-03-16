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
 *
 * A simple, efficient pool for allocating objects of equal size
 */
#include <pthread.h>
#include <crt_util/common.h> /* container_of */
#include <crt_util/list.h>
#include "iof_obj_pool.h"

/* A hack to assert that the sizeof obj_pool_t is large enough */
struct tpv_data {
	struct obj_pool *pool;
	crt_list_t free_entries;
	crt_list_t allocated_blocks;
	crt_list_t link;
};

struct pool_entry {
	union {
		crt_list_t link; /* Free list link */
		char data[0];    /* Data */
	};
};

struct obj_pool {
	pthread_key_t key;           /* key to threadprivate data */
	pthread_mutex_t lock;        /* lock thread events */
	crt_list_t free_entries;     /* entries put in pool by dead thread */
	crt_list_t allocated_blocks; /* blocks allocated by dead thread */
	crt_list_t tpv_list;         /* Threadprivate data */
	size_t obj_size;             /* size of objects in pool */
	size_t padded_size;          /* real size of objects in pool */
	size_t block_size;           /* allocation size */
	int magic;                   /* magic number for sanity */
};

#define PAD8(size) ((size + 7) & ~7)

STATIC_ASSERT(sizeof(obj_pool_t) >= sizeof(struct obj_pool),
	      obj_pool_t_not_big_enough);


#define MAGIC 0x345342aa

/* On thread death, save the free entries globally */
static void save_free_entries(void *tpv_data)
{
	struct tpv_data *tpv = (struct tpv_data *)tpv_data;
	struct obj_pool *pool = tpv->pool;

	if (crt_list_empty(&tpv->free_entries))
		return;

	pthread_mutex_lock(&pool->lock);
	crt_list_splice(&tpv->free_entries, &pool->free_entries);
	crt_list_splice(&tpv->allocated_blocks, &pool->allocated_blocks);
	crt_list_del(&tpv->link);
	pthread_setspecific(pool->key, NULL);
	free(tpv);
	pthread_mutex_unlock(&pool->lock);

}

#define BLOCK_SIZE 16384

/* Initialize an object pool
 * \param pool[out] Pool to initialize
 * \param obj_size[in] Size of objects in pool
 */
int obj_pool_initialize(obj_pool_t *pool, size_t obj_size)
{
	struct obj_pool *real_pool = (struct obj_pool *)pool;
	int rc;

	if (pool == NULL || obj_size == 0)
		return PERR_INVAL;

	if (obj_size > MAX_POOL_OBJ_SIZE)
		return PERR_TOOBIG;


	rc = pthread_key_create(&real_pool->key, save_free_entries);
	if (rc != 0)
		return PERR_NOMEM;

	rc = pthread_mutex_init(&real_pool->lock, NULL);
	if (rc != 0)
		return PERR_NOMEM;

	CRT_INIT_LIST_HEAD(&real_pool->free_entries);
	CRT_INIT_LIST_HEAD(&real_pool->allocated_blocks);
	CRT_INIT_LIST_HEAD(&real_pool->tpv_list);

	real_pool->obj_size = obj_size;
	obj_size = sizeof(struct pool_entry) > obj_size ?
		sizeof(struct pool_entry) : obj_size;
	real_pool->padded_size = PAD8(obj_size);
	real_pool->block_size = (BLOCK_SIZE / real_pool->padded_size) *
				real_pool->padded_size;
	real_pool->magic = MAGIC;

	return 0;
}

/* Destroy a pool and all objects in pool */
int obj_pool_destroy(obj_pool_t *pool)
{
	struct pool_entry *block;
	struct pool_entry *tmpblock;
	struct tpv_data *tpv;
	struct tpv_data *tmptpv;
	struct obj_pool *real_pool = (struct obj_pool *)pool;

	if (pool == NULL)
		return PERR_INVAL;

	if (real_pool->magic != MAGIC)
		return PERR_UNINIT;

	real_pool->magic = 0;

	pthread_key_delete(real_pool->key);

	crt_list_for_each_entry_safe(block, tmpblock,
				     &real_pool->allocated_blocks, link) {
		free(block);
	}
	crt_list_for_each_entry_safe(tpv, tmptpv,
				     &real_pool->tpv_list, link) {
		crt_list_for_each_entry_safe(block, tmpblock,
					     &tpv->allocated_blocks, link) {
			free(block);
		}
		free(tpv);
	}

	pthread_mutex_destroy(&real_pool->lock);

	return 0;
}

static int get_tpv(struct obj_pool *pool, struct tpv_data **tpv)
{
	struct tpv_data *tpv_data = pthread_getspecific(pool->key);

	if (tpv_data == NULL) {
		tpv_data = malloc(sizeof(struct tpv_data));
		if (tpv_data == NULL)
			return PERR_NOMEM;

		CRT_INIT_LIST_HEAD(&tpv_data->free_entries);
		CRT_INIT_LIST_HEAD(&tpv_data->allocated_blocks);
		tpv_data->pool = pool;

		pthread_mutex_lock(&pool->lock);
		crt_list_add(&tpv_data->link, &pool->tpv_list);
		/* Steal entries left by a dead thread */
		if (!crt_list_empty(&pool->free_entries))
			crt_list_splice_init(&pool->free_entries,
					     &tpv_data->free_entries);
		pthread_mutex_unlock(&pool->lock);

		pthread_setspecific(pool->key, tpv_data);
	}

	*tpv = tpv_data;

	return 0;
}

static int get_new_entry(struct pool_entry **entry, struct obj_pool *pool)
{
	char *block;
	char *cursor;
	struct tpv_data *tpv_data;
	int rc;

	rc = get_tpv(pool, &tpv_data);

	if (rc != 0) {
		*entry = NULL;
		return rc;
	}

	if (!crt_list_empty(&tpv_data->free_entries)) {
		*entry = crt_list_entry(tpv_data->free_entries.next,
					struct pool_entry, link);
		crt_list_del(tpv_data->free_entries.next);
		goto zero;
	}

	/* Ok, no entries, let's allocate some and put them in our tpv */
	block = malloc(pool->block_size);
	if (block == NULL) {
		*entry = NULL;
		return PERR_NOMEM;
	}

	/* First entry is reserved for the allocation list */
	/* Give second entry to user */
	*entry = (struct pool_entry *)(block + pool->padded_size);
	/* Put the rest in the tpv free list */
	for (cursor = block + (pool->padded_size * 2);
	     cursor != (block + pool->block_size);
	     cursor += pool->padded_size) {
		crt_list_add((crt_list_t *)cursor, &tpv_data->free_entries);
	}

	crt_list_add((crt_list_t *)block, &tpv_data->allocated_blocks);
zero:
	memset(*entry, 0, pool->padded_size);

	return 0;
}

int obj_pool_get_(obj_pool_t *pool, void **item, size_t size)
{
	struct obj_pool *real_pool = (struct obj_pool *)pool;
	struct pool_entry *entry;
	int rc;

	if (pool == NULL || item == NULL)
		return PERR_INVAL;

	*item = NULL;
	if (real_pool->magic != MAGIC)
		return PERR_UNINIT;

	if (real_pool->obj_size != size)
		return PERR_INVAL;

	rc = get_new_entry(&entry, real_pool);
	if (rc == 0)
		*item = &entry->data[0];

	return rc;
}

int obj_pool_put(obj_pool_t *pool, void *item)
{
	struct obj_pool *real_pool = (struct obj_pool *)pool;
	struct pool_entry *entry;
	struct tpv_data *tpv_data;
	int rc;

	if (pool == NULL || item == NULL)
		return PERR_INVAL;

	if (real_pool->magic != MAGIC)
		return PERR_UNINIT;

	entry = container_of(item, struct pool_entry, data);

	rc = get_tpv(real_pool, &tpv_data);

	if (rc != 0) {
		pthread_mutex_lock(&real_pool->lock);
		crt_list_add(&entry->link, &real_pool->free_entries);
		pthread_mutex_unlock(&real_pool->lock);
		return rc;
	}

	crt_list_add(&entry->link, &tpv_data->free_entries);

	return 0;
}
