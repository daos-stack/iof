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
#ifndef __LOG_H__
#define __LOG_H__
#include <sys/types.h>
#include <stdio.h>

#include <inttypes.h>
#include <gurt/dlog.h>

/* Allow changing the default so these macros can be
 * used by files that don't log to the default facility
 */
#ifndef DEF_LOG_HANDLE
#define DEF_LOG_HANDLE iof_log_handle
#endif

#define IOF_LOG_FAC(fac, type, fmt, ...)				\
	do {								\
		d_log(fac | DLOG_##type, "%s:%d %s() " fmt "\n",	\
		      __FILE__,  __LINE__, __func__,			\
		      ## __VA_ARGS__);					\
	} while (0)

#define IOF_LOG_WARNING(...)	\
	IOF_LOG_FAC(DEF_LOG_HANDLE, WARN, __VA_ARGS__)

#define IOF_LOG_ERROR(...)	\
	IOF_LOG_FAC(DEF_LOG_HANDLE, ERR, __VA_ARGS__)

#define IOF_LOG_DEBUG(...)	\
	IOF_LOG_FAC(DEF_LOG_HANDLE, DBG, __VA_ARGS__)

#define IOF_LOG_INFO(...)	\
	IOF_LOG_FAC(DEF_LOG_HANDLE, INFO, __VA_ARGS__)

/* IOF_TRACE marcos defined for tracing descriptors and RPCs
 * in the logs. UP() is used to register a new descriptor -
 * this includes giving it a "type" and also a parent to build
 * a descriptor hierarchy. Then DOWN() will de-register
 * the descriptor.
 *
 * For RPCs only, LINK() is used to link an RPC to a
 * descriptor in the hierarchy. RPCs are not registered
 * (warning if UP and LINK are both called for the same pointer).
 *
 * All other logging remains the same for WARNING/ERROR/
 * DEBUG/INFO, however just takes an extra argument for the
 * lowest-level descriptor to tie the logging message to.
 */
#define IOF_TRACE(ptr, fac, type, fmt, ...)		\
	d_log(fac | DLOG_##type,			\
		"%s:%d TRACE: %s(%p) " fmt "\n",	\
		__FILE__,  __LINE__, __func__, ptr,	\
		## __VA_ARGS__)

#define IOF_TRACE_WARNING(ptr, ...)			\
	IOF_TRACE(ptr, DEF_LOG_HANDLE, WARN, __VA_ARGS__)

#define IOF_TRACE_ERROR(ptr, ...)			\
	IOF_TRACE(ptr, DEF_LOG_HANDLE, ERR, __VA_ARGS__)

#define IOF_TRACE_DEBUG(ptr, ...)			\
	IOF_TRACE(ptr, DEF_LOG_HANDLE, DBG, __VA_ARGS__)

#define IOF_TRACE_INFO(ptr, ...)			\
	IOF_TRACE(ptr, DEF_LOG_HANDLE, INFO, __VA_ARGS__)

/* Register a descriptor with a parent and a type */
#define IOF_TRACE_UP(ptr, parent, type)					\
	d_log(DEF_LOG_HANDLE | DLOG_DBG,				\
		"%s:%d TRACE: %s(%p) Registered new '%s' from %p\n",	\
		__FILE__, __LINE__, __func__, ptr, type, parent)

/* Create an alias for an already registered descriptor with a new
 * type and parent
 */
#define IOF_TRACE_ALIAS(ptr, parent, type)				\
	d_log(DEF_LOG_HANDLE | DLOG_DBG,				\
		"%s:%d TRACE: %s(%p) Alias '%s' from %p\n",		\
		__FILE__, __LINE__, __func__, ptr, type, parent)

/* Link an RPC to a descriptor */
#define IOF_TRACE_LINK(ptr, parent, type)				\
	d_log(DEF_LOG_HANDLE | DLOG_DBG,				\
		"%s:%d TRACE: %s(%p) Link '%s' to %p\n",		\
		__FILE__, __LINE__, __func__, ptr, type, parent)

/* De-register a descriptor, including all aliases */
#define IOF_TRACE_DOWN(ptr)						\
	d_log(DEF_LOG_HANDLE | DLOG_DBG,				\
		"%s:%d TRACE: %s(%p) Deregistered\n",			\
		__FILE__, __LINE__, __func__, ptr)

/* Register as root of hierarchy, used in place of IOF_TRACE_UP */
#define IOF_TRACE_ROOT(ptr, type)				\
	d_log(DEF_LOG_HANDLE | DLOG_DBG,			\
		"%s:%d TRACE: %s(%p) Registered new %s as root\n",\
		__FILE__, __LINE__, __func__, ptr, type)

#if defined(__cplusplus)
extern "C" {
#endif

extern int iof_log_handle;

/* Initialize a log facility.   Pass NULL as log handle to initialize
 * the default facilility.  If the user never initializes the default
 * facility, it will be set to whatever is passed in first.
 */
void iof_log_init(const char *shortname, const char *longname,
		  int *log_handle);
/* Close a the log for a log facility */
void iof_log_close(void);

#if defined(__cplusplus)
}
#endif
#endif /* __LOG_H__ */
