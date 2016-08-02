
#include "iof_plugin.h"

int iof_plugin_init(struct cnss_plugin **fns, size_t *size);

struct cnss_state {
	struct mcl_state *mcl_state;
	struct mcl_context *context;
	hg_id_t projection_query;
};

