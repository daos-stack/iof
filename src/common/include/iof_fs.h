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
#ifndef __IOF_FS_H__
#define __IOF_FS_H__

#include <crt_types.h>
#include <iof_atomic.h>
#include <ios_gah.h>

struct iof_service_group {
	crt_group_t		*dest_grp; /* Server group */
	crt_endpoint_t		psr_ep;    /* Server PSR endpoint */
	uint32_t		grp_id;    /* CNSS defined ionss id */
	bool			enabled;   /* Indicates group is available */
};

/* For each projection */
struct iof_projection {
	struct iof_service_group	*grp;      /* Server group info */
	crt_context_t			*crt_ctx;  /* context to use */
	uint32_t			grp_id;    /* CNSS defined ionss id */
	int				cli_fs_id; /* client projection id */
	bool				enabled;   /* Projection enabled */
};

/* Common data stored on open file handles */
struct iof_file_common {
	struct iof_projection	*projection;
	struct ios_gah		gah;
	crt_endpoint_t		ep;
	int			gah_valid;
};

/*
 * This will be defined by the calling function to select
 * the correct RPC type from the protocol registry.
 * This is used in the FS_TO_OP Macro below.
 */
#ifndef IOF_PROTO_CLASS
#define IOF_PROTO_CLASS DEFAULT
#endif

/*
 * Helpers for forcing macro expansion.
 */
#define EVAL_PROTO_CLASS(CLS) DEF_PROTO_CLASS(CLS)
#define EVAL_RPC_TYPE(CLS, TYPE) DEF_RPC_TYPE(CLS, TYPE)
/*
 * Returns the correct RPC Type ID from the protocol registry.
 */
#define FS_TO_OP(HANDLE, FN) \
		((&iof_protocol_registry[EVAL_PROTO_CLASS(IOF_PROTO_CLASS)])\
		  ->rpc_types[EVAL_RPC_TYPE(IOF_PROTO_CLASS, FN)].op_id)

/* Tracks remaining events for completion */
struct iof_tracker {
	ATOMIC int remaining;
};

/* Initialize number of events to track */
static inline void iof_tracker_init(struct iof_tracker *tracker,
				    int expected_count)
{
	atomic_store_release(&tracker->remaining, expected_count);
}

/* Signal an event */
static inline void iof_tracker_signal(struct iof_tracker *tracker)
{
	atomic_dec_release(&tracker->remaining);
}

/* Test if all events have signaled */
static inline bool iof_tracker_test(struct iof_tracker *tracker)
{
	if (atomic_load_consume(&tracker->remaining) == 0)
		return true;

	return false;
}

/* Progress until all events have signaled */
void iof_wait(crt_context_t, struct iof_tracker *);

/* Progress until all events have signaled */
#define iof_fs_wait(fs_handle, event) \
	iof_wait((fs_handle)->crt_ctx, event)

#endif