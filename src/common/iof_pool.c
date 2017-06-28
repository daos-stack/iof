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

#include <string.h>

#include "iof_pool.h"
#include "log.h"

int iof_pool_init(struct iof_pool *pool)
{
	IOF_LOG_DEBUG("Created a pool at %p", pool);
	CRT_INIT_LIST_HEAD(&pool->list);

	pthread_mutex_init(&pool->lock, NULL);
	return 0;
}

void iof_pool_destroy(struct iof_pool *pool)
{
	struct iof_pool_type *type, *tnext;

	iof_pool_reclaim(pool);
	crt_list_for_each_entry_safe(type, tnext, &pool->list, type_list) {
		IOF_LOG_DEBUG("Freeing type %p", type);
		free(type);
	}
}

void iof_pool_reclaim(struct iof_pool *pool)
{
	struct iof_pool_type *type;

	pthread_mutex_lock(&pool->lock);
	crt_list_for_each_entry(type, &pool->list, type_list) {
		crt_list_t *entry, *enext;

		IOF_LOG_DEBUG("Cleaning type %p", type);

		iof_pool_restock(type);
		pthread_mutex_lock(&type->lock);

		crt_list_for_each_safe(entry, enext, &type->free_list) {
			void *ptr = (void *)entry - type->reg.offset;

			if (type->reg.release)
				type->reg.release(ptr);

			crt_list_del(entry);
			free(ptr);
			type->count--;
		}
		IOF_LOG_DEBUG("%d in use", type->count);
	}
}

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

	pthread_mutex_lock(&pool->lock);
	crt_list_add_tail(&type->type_list, &pool->list);
	pthread_mutex_unlock(&pool->lock);

	return type;
}

void *
iof_pool_acquire(struct iof_pool_type *type)
{
	void *ptr;
	crt_list_t *entry;

	pthread_mutex_lock(&type->lock);

	if (!crt_list_empty(&type->free_list)) {
		entry = type->free_list.next;
		crt_list_del_init(entry);
		ptr = (void *)entry - type->reg.offset;
	} else {
		int rc;

		ptr = calloc(1, type->reg.size);
		if (!ptr)
			goto out;
		if (type->reg.init) {
			rc = type->reg.init(ptr, type->reg.handle);
			if (rc != 0) {
				free(ptr);
				ptr = NULL;
				goto out;
			}
		}

		if (type->reg.clean) {
			rc = type->reg.clean(ptr);
			if (rc != 0) {
				free(ptr);
				ptr = NULL;
				goto out;
			}
		}
		type->count++;
	}

out:

	pthread_mutex_unlock(&type->lock);

	IOF_LOG_DEBUG("Type %p Using %p", type, ptr);
	return ptr;
}

void
iof_pool_release(struct iof_pool_type *type, void *ptr)
{
	crt_list_t *entry = ptr + type->reg.offset;

	IOF_LOG_DEBUG("Releasing %p", ptr);
	pthread_mutex_lock(&type->lock);
	crt_list_add_tail(entry, &type->pending_list);
	pthread_mutex_unlock(&type->lock);
}

void
iof_pool_restock(struct iof_pool_type *type)
{
	crt_list_t *entry, *enext;

	IOF_LOG_DEBUG("Restocking %p count %d", type, type->count);

	pthread_mutex_lock(&type->lock);

	crt_list_for_each_safe(entry, enext, &type->pending_list) {
		void *ptr = (void *)entry - type->reg.offset;
		int rc;

		IOF_LOG_DEBUG("Cleaning %p", ptr);

		crt_list_del(entry);
		if (type->reg.clean) {
			rc = type->reg.clean(ptr);
			if (rc == 0) {
				crt_list_add(entry, &type->free_list);
			} else {
				IOF_LOG_DEBUG("entry failed clean %p", ptr);
				type->count--;
				free(entry);
			}
		} else {
			crt_list_add(entry, &type->free_list);
		}
	}

	pthread_mutex_unlock(&type->lock);
}
