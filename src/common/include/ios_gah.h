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
#ifndef __IOF_GAH_H__
#define __IOF_GAH_H__

#include <inttypes.h>
#include <gurt/list.h>

enum ios_return {
	IOS_SUCCESS = 0,
	IOS_ERR_INVALID_PARAM,		/**< Invalid parameter */
	IOS_ERR_CRC_MISMATCH,		/**< CRC mismatch */
	IOS_ERR_VERSION_MISMATCH,	/**< version mismatch */
	IOS_ERR_NOMEM,			/**< Memory not available */
	IOS_ERR_OUT_OF_RANGE,		/**< Handle ID out of range */
	IOS_ERR_EXPIRED,		/**< Handle has expired */
	IOS_ERR_OTHER			/**< All other errors */
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
/** 128 bit GAH handle */
struct ios_gah {
	uint64_t revision:48;	/**< 0-based revision NO. of the fid */
	uint8_t root;		/**< The rank who owns the GAH */
	uint8_t base;		/**< Rank who owns the first byte of the file */
	uint8_t version;	/**< The version of the protocol */
	uint64_t fid:24;	/**< The file id of the file*/
	uint64_t reserved:24;	/**< Reserved for future use */
	uint8_t crc;		/**< CRC of the rest of the structure */
};
#pragma GCC diagnostic pop

/** metadata associated with the file */
struct ios_gah_ent {
	void *internal;
	d_list_t list;
	uint64_t in_use;
	uint64_t revision;
	uint64_t fid;	/**< The file id of the file*/
};

/**
 * structure with dynamically-sized storage to keep the file metadata.
 */
struct ios_gah_store {
	/** number of used slots */
	int size;
	/** total number of slots including used and unused */
	int capacity;
	/** storage for the actual file entries */
	struct ios_gah_ent *data;
	/** array of pointers to file entries */
	struct ios_gah_ent **ptr_array;

	/** list of available file entries */
	d_list_t free_list;
};

/**
 * initialize struct ios_gah_ent ios_gah_store.
 */
struct ios_gah_store *ios_gah_init(void);

/**
 * \param ios_gah_store [IN]		Global access handle data structure.
 *
 * return				On success, returns IOS_SUCCESS
 */
enum ios_return ios_gah_destroy(struct ios_gah_store *ios_gah_store);

/**
 * Allocates a global access handle, computes the crc.
 *
 * \param ios_gah_store [IN]		Global access handle data structure.
 * \param gah		[OUT]		On exit, *gah contains a global access
 *					handle.
 * \param self_rank	[IN]		Rank of the calling process.
 * \param base		[IN]		Rank of the process which serves the
 *					first byte of the file.
 *
 * \return				On success, returns IOS_SUCCESS.
 */
enum ios_return ios_gah_allocate(struct ios_gah_store *ios_gah_store,
		struct ios_gah *gah, int self_rank, int base, void *internal);

/**
 * Deallocates a global access handle.
 *
 * \param ios_gah_store [IN]		Global access handle data structure.
 * \param gah		[IN/OUT]	On exit, the global access handle
 *					contained in *gah is marked available
 *					again.
 *
 * \return				On success, returns IOS_SUCCESS.
 */
enum ios_return ios_gah_deallocate(struct ios_gah_store *ios_gah_store,
		struct ios_gah *gah);

/**
 * Retrieve opaque data structure corresponding to a given global access handle.
 *
 * \param gah_store [IN]		Global access handle data structure
 * \param gah [IN]			Global access handle
 * \param info [OUT]			On success, *info contains the opaque
 *					data struture associated with gah. On
 *					failure, equals NULL
 *
 * \return				On success returns IOS_SUCCESS
 */
enum ios_return ios_gah_get_info(struct ios_gah_store *gah_store,
		struct ios_gah *gah, void **internal);
/**
 * Validates if the crc in *gah is correct.
 *
 * \param gah		[IN]		On entry, *gah contains a global access
 *					handle.
 * \return				crc is correct ---  returns 0
 *					crc is incorrect ---  returns 1
 *					NULL input --- returns
 *					IOS_ERR_INVALID_PARAM
 */
enum ios_return ios_gah_check_crc(struct ios_gah *gah);

/**
 * Validates if the version in *gah and the and the version of the protocol in
 * use match.
 *
 * \param gah		[IN]		On entry, *gah contains a global access
 *					handle.
 * \return				version match ---  returns 0
 *					version mismatch ---  returns 1
 *					NULL input --- returns
 *					IOS_ERR_INVALID_PARAM
 */
enum ios_return ios_gah_check_version(struct ios_gah *gah);

/**
 * Check if the calling process is the root of the global access handle.
 *
 * \param gah		[IN]		On entry, contains a global access
 *					handle.
 * \param self_rank	[IN]		The rank of the calling process.
 *
 * \return				Returns 0 if I am the root, 1 if I am
 *					not.
 */
enum ios_return ios_gah_is_self_root(struct ios_gah *gah, int self_rank);

#define GAH_PRINT_STR "Gah(%" PRIu8 ".%" PRIu32 ".%" PRIu64 ")"
#define GAH_PRINT_VAL(P) (P).root, (P).fid, (uint64_t)(P).revision

#define GAH_PRINT_FULL_STR GAH_PRINT_STR " revision: %" PRIu64 " root: %" \
	PRIu8 " base: %" PRIu8 " version: %" PRIu8 " fid: %" PRIu32

#define GAH_PRINT_FULL_VAL(P) GAH_PRINT_VAL(P), (uint64_t)(P).revision, \
		(P).root, (P).base, (P).version, (P).fid

#endif
