#ifndef __LOG_H__
#define __LOG_H__
#include <mcl_log.h>

#define IOF_LOG_DEBUG(format, ...)	\
	MCL_LOG_DEBUG(iof_log_handle, (format), ## __VA_ARGS__)

#define IOF_LOG_INFO(format, ...)	\
	MCL_LOG_INFO(iof_log_handle, (format), ## __VA_ARGS__)

#define IOF_LOG_WARNING(format, ...)	\
	MCL_LOG_WARNING(iof_log_handle, (format), ## __VA_ARGS__)

#define IOF_LOG_ERROR(format, ...)	\
	MCL_LOG_ERROR(iof_log_handle, (format), ## __VA_ARGS__)

#if defined(__cplusplus)
extern "C" {
#endif

extern int iof_log_handle;

void iof_log_init(const char *component);
void iof_log_close(void);

#if defined(__cplusplus)
}
#endif
#endif /* __LOG_H__ */
