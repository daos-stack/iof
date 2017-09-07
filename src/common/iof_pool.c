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

#include <stdlib.h>
#include <string.h>

#include "iof_pool.h"
#include "log.h"

static void
debug_dump(struct iof_pool_type *type)
{
	IOF_LOG_INFO("Pool type %p", type);
	IOF_LOG_DEBUG("handle %p size %d offset %d", type->reg.handle,
		      type->reg.size, type->reg.offset);
	IOF_LOG_DEBUG("Count: free %d pending %d total %d", type->free_count,
		      type->pending_count, type->count);
	IOF_LOG_DEBUG("Calls: init %d clean %d release %d", type->init_count,
		      type->clean_count, type->release_count);
	IOF_LOG_DEBUG("OP: init %d clean %d", type->op_init, type->op_clean);
	IOF_LOG_DEBUG("No restock: current %d hwm %d", type->no_restock,
		      type->no_restock_hwm);
}

/* Create a object pool */
int
iof_pool_init(struct iof_pool *pool)
{
	IOF_LOG_DEBUG("Created a pool at %p", pool);
	CRT_INIT_LIST_HEAD(&pool->list);

	pthread_mutex_init(&pool->lock, NULL);
	return 0;
}

/* Destroy a object pool */
void
iof_pool_destroy(struct iof_pool *pool)
{
	struct iof_pool_type *type, *tnext;

	crt_list_for_each_entry(type, &pool->list, type_list) {
		debug_dump(type);
	}

	iof_pool_reclaim(pool);
	crt_list_for_each_entry_safe(type, tnext, &pool->list, type_list) {
		IOF_LOG_DEBUG("Freeing type %p", type);
		if (type->count != 0)
			IOF_LOG_WARNING("Freeing type with active objects");
		free(type);
	}
}

/* Helper function for migrating objects from pending list to free list.
 *
 * Migrates objects from the pending list to the free list.  Keeps going
 * until either there are count objects on the free list or there are
 * no more pending objects;
 * This function should be called with the type lock held.
 */
static int
restock(struct iof_pool_type *type, int count)
{
	crt_list_t *entry, *enext;
	int clean_calls = 0;

	if (type->free_count >= count)
		return 0;

	crt_list_for_each_safe(entry, enext, &type->pending_list) {
		void *ptr = (void *)entry - type->reg.offset;
		int rc;

		IOF_LOG_DEBUG("Cleaning %p", ptr);

		crt_list_del(entry);
		type->pending_count--;

		if (type->reg.clean) {
			type->clean_count++;
			clean_calls++;
			rc = type->reg.clean(ptr);
			if (rc == 0) {
				crt_list_add(entry, &type->free_list);
				type->free_count++;
			} else {
				IOF_LOG_DEBUG("entry failed clean %p", ptr);
				type->count--;
				free(ptr);
			}
		} else {
			crt_list_add(entry, &type->free_list);
			type->free_count++;
		}
		if (type->free_count == count)
			return clean_calls;
	}
	return clean_calls;
}

/* Reclaim any memory possible */
void
iof_pool_reclaim(struct iof_pool *pool)
{
	struct iof_pool_type *type;

	pthread_mutex_lock(&pool->lock);
	crt_list_for_each_entry(type, &pool->list, type_list) {
		crt_list_t *entry, *enext;

		IOF_LOG_DEBUG("Cleaning type %p", type);

		pthread_mutex_lock(&type->lock);

		/* Reclaim any pending objects.  Count here just needs to be
		 * larger than pending_count + free_count however simply
		 * using count is adequate as is guaranteed to be larger.
		 */
		restock(type, type->count);

		crt_list_for_each_safe(entry, enext, &type->free_list) {
			void *ptr = (void *)entry - type->reg.offset;

			if (type->reg.release) {
				type->reg.release(ptr);
				type->release_count++;
			}

			IOF_LOG_INFO("Destroying object at %p", ptr);
			crt_list_del(entry);
			free(ptr);
			type->free_count--;
			type->count--;
		}
		IOF_LOG_DEBUG("%d in use", type->count);
	}
}

/* Create a single new object
 *
 * Returns a pointer to the object or NULL if allocation fails.
 */
static void *
create(struct iof_pool_type *type)
{
	void *ptr = calloc(1, type->reg.size);
	int rc;

	if (!ptr)
		return NULL;

	type->init_count++;
	if (type->reg.init) {
		rc = type->reg.init(ptr, type->reg.handle);

		if (rc != 0) {
			free(ptr);
			return NULL;
		}
	}

	if (type->reg.clean) {
		rc = type->reg.clean(ptr);
		if (rc != 0) {
			free(ptr);
			return NULL;
		}
	}
	type->count++;

	IOF_LOG_INFO("Created new object type %p at %p", type, ptr);
	return ptr;
}

/* Populate the free list
 *
 * Create objects and add them to the free list.  Creates one more object
 * than is needed to ensure that if the HWM of no-restock calls is reached
 * there will be no on-path allocations.
 */
static void
create_many(struct iof_pool_type *type)
{
	while (type->free_count < (type->no_restock_hwm + 1)) {
		void *ptr = create(type);
		crt_list_t *entry = ptr + type->reg.offset;

		if (!ptr)
			return;

		crt_list_add_tail(entry, &type->free_list);
		type->free_count++;
	}
}

/* Register a pool type */
struct iof_pool_type *
iof_pool_register(struct iof_pool *pool, struct iof_pool_reg *reg)
{
	struct iof_pool_type *type;

	type = calloc(1, sizeof(*type));
	if (!type)
		return NULL;

	IOF_LOG_DEBUG("Pool %p create a type at %p", pool, type);

	pthread_mutex_init(&type->lock, NULL);
	CRT_INIT_LIST_HEAD(&type->free_list);
	CRT_INIT_LIST_HEAD(&type->pending_list);

	type->count = 0;
	memcpy(&type->reg, reg, sizeof(*reg));

	create_many(type);

	pthread_mutex_lock(&pool->lock);
	crt_list_add_tail(&type->type_list, &pool->list);
	pthread_mutex_unlock(&pool->lock);

	return type;
}

/* Acquire a new object.
 *
 * This is to be considered on the critical path so should be as lightweight
 * as posslble.
 */
void *
iof_pool_acquire(struct iof_pool_type *type)
{
	void *ptr = NULL;
	crt_list_t *entry;

	pthread_mutex_lock(&type->lock);

	type->no_restock++;

	if (type->free_count == 0) {
		int count = restock(type, 1);

		type->op_clean += count;
	}

	if (!crt_list_empty(&type->free_list)) {
		entry = type->free_list.next;
		crt_list_del(entry);
		entry->next = NULL;
		entry->prev = NULL;
		type->free_count--;
		ptr = (void *)entry - type->reg.offset;
	} else {
		if (!type->reg.max_desc || type->count < type->reg.max_desc) {
			type->op_init++;
			ptr = create(type);
		}
	}

	pthread_mutex_unlock(&type->lock);

	if (ptr)
		IOF_LOG_DEBUG("Type %p Using %p", type, ptr);
	else
		IOF_LOG_WARNING("Failed to allocate for type %p", type);
	return ptr;
}

/* Release an object ready for reuse
 *
 * This is sometimes on the critical path, sometimes not so assume that
 * for all cases it is.
 *
 */
void
iof_pool_release(struct iof_pool_type *type, void *ptr)
{
	crt_list_t *entry = ptr + type->reg.offset;

	IOF_LOG_DEBUG("Releasing %p", ptr);
	pthread_mutex_lock(&type->lock);
	type->pending_count++;
	crt_list_add_tail(entry, &type->pending_list);
	pthread_mutex_unlock(&type->lock);
}

/* Re-stock a object type.
 *
 * This is a function called off the critical path to pre-alloc and recycle
 * objects to be ready for re-use.  In an ideal world this function does
 * all the heavy lifting and acquire/release are very cheap.
 *
 * Ideally this function should be called once for every acquire(), after the
 * object has been used however correctness is maintained even if that is not
 * the case.
 */
void
iof_pool_consume(struct iof_pool_type *type, void *ptr)
{
	IOF_LOG_DEBUG("Marking %p as consumed", ptr);
	pthread_mutex_lock(&type->lock);
	type->count--;
	pthread_mutex_unlock(&type->lock);
}

void
iof_pool_restock(struct iof_pool_type *type)
{
	IOF_LOG_DEBUG("Restocking %p count (%d/%d/%d)", type,
		      type->pending_count, type->free_count, type->count);

	pthread_mutex_lock(&type->lock);

	/* Update restock hwm metrics */
	if (type->no_restock > type->no_restock_hwm)
		type->no_restock_hwm = type->no_restock;
	type->no_restock = 0;

	/* Move from pending to free list */
	restock(type, type->no_restock_hwm + 1);

	if (!type->reg.max_desc)
		create_many(type);

	pthread_mutex_unlock(&type->lock);
}
