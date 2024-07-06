#include <linux/nomad.h>

struct async_promote_ctrl async_mod_glob_ctrl = {
	.initialized = 0,
	.queue_page_fault = NULL,

	.link_shadow_page = NULL,
	.demote_shadow_page_find = NULL,
	.demote_shadow_page_breakup = NULL,
	.release_shadow_page = NULL,
	.reclaim_page = NULL,
};
EXPORT_SYMBOL(async_mod_glob_ctrl);
