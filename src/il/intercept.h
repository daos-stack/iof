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
#ifndef __INTERCEPT_H__
#define __INTERCEPT_H__
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

/* TODO: We need to figure out how to handle logging.  We may need
 * to make sure CART logging turns itself off inside a log routine
 * or we can get an infinite recursion.  For now, this macro is
 * added for debugging
 */
/* #define IOIL_DEBUG */
#ifdef IOIL_DEBUG
#define DEBUG_PRINT(...) printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(...) (void)0
#endif

#ifdef IOIL_PRELOAD
#include <dlfcn.h>

#define IOIL_FORWARD_DECL(type, name, args)  \
	static type (*__real_##name) args

#define IOIL_DECL(name) name

/* Initialize the __real_##name function pointer */
#define IOIL_FORWARD_MAP_OR_FAIL(name)                                      \
	do {                                                                \
		if (__real_##name != NULL)                                  \
			break;                                              \
		__real_##name = (__typeof__(__real_##name))dlsym(RTLD_NEXT, \
								 #name);    \
		if (__real_ ## name == NULL) {                              \
			fprintf(stderr,                                     \
				"libiofil couldn't map " #name "\n");       \
			exit(1);                                            \
		}                                                           \
	} while (0)

#else /* !IOIL_PRELOAD */
#define IOIL_FORWARD_DECL(type, name, args)  \
	extern type __real_##name args

#define IOIL_DECL(name) __wrap_##name

#define IOIL_FORWARD_MAP_OR_FAIL(name) (void)0

#endif /* IOIL_PRELOAD */

#endif /* __INTERCEPT_H__ */
