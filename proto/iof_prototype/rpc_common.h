#include<mercury.h>
#include<mercury_bulk.h>
#include<mercury_macros.h>
#include<mercury_bulk.h>

/* Defines generic functions used across all callbacks and all
 * mercury related headers
 */
#ifndef RPC_COMMON_H
#define RPC_COMMON_H

hg_class_t *engine_init(na_bool_t listen, const char *local_addr, int start_thread, na_class_t **network_class);
void engine_finalize(void);
void engine_create_handle(na_addr_t addr, hg_id_t id, hg_handle_t *handle);
hg_return_t engine_progress(int *done);

#endif
