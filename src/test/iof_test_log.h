#ifndef __IOFTESTLOG_H__
#define __IOFTESTLOG_H__
#include <mcl_log.h>

#define IOF_TESTLOG_DEBUG(...)	\
	MCL_LOG_DEBUG(iof_testlog_handle, __VA_ARGS__)

#define IOF_TESTLOG_INFO(...)	\
	MCL_LOG_INFO(iof_testlog_handle, __VA_ARGS__)

#define IOF_TESTLOG_WARNING(...)	\
	MCL_LOG_WARNING(iof_testlog_handle, __VA_ARGS__)

#define IOF_TESTLOG_ERROR(...)	\
	MCL_LOG_ERROR(iof_testlog_handle, __VA_ARGS__)

#if defined(__cplusplus)
extern "C" {
#endif

extern int iof_testlog_handle;

void iof_testlog_init(const char *component);
void iof_testlog_close(void);

#if defined(__cplusplus)
}
#endif
#endif /* __IOFTESTLOG_H__ */
