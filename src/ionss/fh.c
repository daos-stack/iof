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

#include "iof_common.h"
#include "ionss.h"
#include "log.h"

int ios_fh_alloc(struct ios_projection *projection,
		 struct ionss_file_handle **fhp)
{
	struct ios_base *base;
	struct ionss_file_handle *fh;
	int rc;

	*fhp = NULL;

	if (LIST_EMPTY(&projection->inactive_files)) {
		fh = calloc(1, sizeof(*fh));
		if (!fh) {
			IOF_LOG_ERROR("Failed to allocate memory");
			return IOF_ERR_NOMEM;
		}
		fh->projection = projection;
	} else {
		fh = LIST_FIRST(&projection->inactive_files);
		LIST_REMOVE(fh, list);
	}

	base = projection->base;

	rc = ios_gah_allocate(base->gs, &fh->gah, 0, 0, fh);
	if (rc) {
		IOF_LOG_ERROR("Failed to acquire GAH %d", rc);
		free(fh);
		return IOS_ERR_NOMEM;
	}

	fh->ref = 1;
	*fhp = fh;

	IOF_LOG_INFO("Handle %p " GAH_PRINT_FULL_STR, fh,
		     GAH_PRINT_FULL_VAL(fh->gah));

	return IOS_SUCCESS;
}

void ios_fh_decref(struct ionss_file_handle *fh, int count)
{
	struct ios_projection *projection;
	struct ios_base *base;
	uint oldref;
	int rc;

	oldref = atomic_fetch_sub(&fh->ref, count);
	if (oldref != count) {
		IOF_LOG_DEBUG("Keeping " GAH_PRINT_STR " ref %d",
			      GAH_PRINT_VAL(fh->gah), fh->ref);
		return;
	}

	IOF_LOG_INFO("Dropping " GAH_PRINT_STR, GAH_PRINT_VAL(fh->gah));

	rc = close(fh->fd);
	if (rc != 0)
		IOF_LOG_ERROR("Failed to close file %d", fh->fd);

	projection = fh->projection;
	base = projection->base;

	rc = ios_gah_deallocate(base->gs, &fh->gah);
	if (rc)
		IOF_LOG_ERROR("Failed to deallocate GAH %d", rc);

	LIST_REMOVE(fh, list);

	if (LIST_EMPTY(&projection->inactive_files))
		LIST_INSERT_HEAD(&projection->inactive_files, fh, list);
	else
		free(fh);
}

/* Try to ensure that there is at least one pre-allocated file_handle
 * available on the free list.
 * Called without the projection lock held.
 */
void ios_fh_prealloc(struct ios_projection *projection)
{
	struct ionss_file_handle *fh;

	if (!LIST_EMPTY(&projection->inactive_files))
		return;

	fh = calloc(1, sizeof(*fh));
	if (!fh) {
		IOF_LOG_ERROR("Failed to allocate memory");
		return;
	}

	fh->projection = projection;

	pthread_mutex_lock(&projection->lock);
	LIST_INSERT_HEAD(&projection->inactive_files, fh, list);
	pthread_mutex_unlock(&projection->lock);

}

struct ionss_file_handle *ios_fh_find_real(struct ios_base *base,
		  struct ios_gah *gah,
		  const char *fn)
{
	struct ionss_file_handle *fh = NULL;
	uint oldref;
	int rc;

	rc = ios_gah_get_info(base->gs, gah, (void **)&fh);
	if (rc || !fh) {
		IOF_LOG_ERROR("Failed to load fh from " GAH_PRINT_FULL_STR,
			      GAH_PRINT_FULL_VAL(*gah));
		return NULL;
	}

	oldref = atomic_fetch_add(&fh->ref, 1);

	IOF_LOG_DEBUG("%s() Found " GAH_PRINT_STR " ref %d",
		      fn, GAH_PRINT_VAL(fh->gah), oldref);

	return fh;
}

struct ionss_dir_handle *ios_dirh_find_real(struct ios_base *base,
		  struct ios_gah *gah,
		  const char *fn)
{
	struct ionss_dir_handle *dirh = NULL;
	int rc;

	rc = ios_gah_get_info(base->gs, gah, (void **)&dirh);
	if (rc || !dirh) {
		IOF_LOG_ERROR("Failed to load dirh from " GAH_PRINT_FULL_STR,
			      GAH_PRINT_FULL_VAL(*gah));
		return NULL;
	}

	IOF_LOG_DEBUG("%s() Found " GAH_PRINT_STR,
		      fn, GAH_PRINT_VAL(*gah));

	return dirh;
}
