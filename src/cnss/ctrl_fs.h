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
#ifndef __CTRL_FS_H__
#define __CTRL_FS_H__

#if defined(__cplusplus)
extern "C" {
#endif

#include "cnss_plugin.h"

/* Starts the control file system, mounted at <prefix> */
int ctrl_fs_start(const char *prefix);
/* Signals to stop the control file system */
int ctrl_fs_stop(void);
/* Waits for the control file system to stop */
int ctrl_fs_wait(void);
/* Register a control variable.
 * \param[in] path Path to file representing the variable
 * \param[in] read_cb Optional callback to populate the value on read
 * \param[in] write_cb Optional callback to consume the value on write
 * \param[in] destroy_cb Optional callback to free associated data on exit
 * \param[in] cb_arg Optional argument to pass to callbacks
 */
int ctrl_register_variable(const char *path, ctrl_fs_read_cb_t read_cb,
			   ctrl_fs_write_cb_t write_cb,
			   ctrl_fs_destroy_cb_t destroy_cb, void *cb_arg);
/* Register a control event.
 * \param[in] path Path to file representing the event
 * \param[in] trigger_cb Optional callback to invoke when the file is touched
 * \param[in] destroy_cb Optional callback to free associated data on exit
 * \param[in] cb_arg Optional argument to pass to callbacks
 */
int ctrl_register_event(const char *path, ctrl_fs_trigger_cb_t trigger_cb,
			ctrl_fs_destroy_cb_t destroy_cb, void *cb_arg);
/* Register a control constant.
 * \param[in] path Path to file representing the constant
 * \param[in] value Contents of the file
 */
int ctrl_register_constant(const char *path, const char *value);
/* Register a control counter
 * \param[in] path Path to file representing the counter
 * \param[in] open_cb Optional callback to invoke when the file opened
 * \param[in] close_cb Optional callback to invoke when the file closed
 * \param[in] destroy_cb Optional callback to free associated data on exit
 * \param[in] cb_arg Optional argument to pass to callbacks
 */
int ctrl_register_counter(const char *path, int start, int increment,
			  ctrl_fs_open_cb_t open_cb,
			  ctrl_fs_close_cb_t close_cb,
			  ctrl_fs_destroy_cb_t destroy_cb, void *cb_arg);

#if defined(__cplusplus)
extern "C" }
#endif

#endif /* __CTRL_FS_H__ */