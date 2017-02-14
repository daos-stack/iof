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
#ifndef __LOG_H__
#define __LOG_H__
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <crt_util/clog.h>

#define IOF_LOG_FAC(fac, type, fmt, ...)				\
	do {								\
		crt_log(fac | CLOG_##type, "%s:%d %s() " fmt "\n",	\
			__FILE__,  __LINE__, __func__,			\
			## __VA_ARGS__);				\
	} while (0)

#define IOF_LOG_WARNING(...)	\
	IOF_LOG_FAC(iof_log_handle, WARN, __VA_ARGS__)

#define IOF_LOG_ERROR(...)	\
	IOF_LOG_FAC(iof_log_handle, ERR, __VA_ARGS__)

#define IOF_LOG_DEBUG(...)	\
	IOF_LOG_FAC(iof_log_handle, DBG, __VA_ARGS__)

#define IOF_LOG_INFO(...)	\
	IOF_LOG_FAC(iof_log_handle, INFO, __VA_ARGS__)


#if defined(__cplusplus)
extern "C" {
#endif

extern int iof_log_handle;

void iof_log_init(const char *shortname, const char *longname);
void iof_log_close(void);

#if defined(__cplusplus)
}
#endif
#endif /* __LOG_H__ */
