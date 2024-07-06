#ifndef _LINUX_NOMAD_H
#define _LINUX_NOMAD_H
#include <linux/rmap.h>
#include <linux/types.h>
#include <linux/pgtable.h>

enum nomad_stat_async_promotion {
	NOMAD_MODULE_NOT_ENABLED,
	NOMAD_SYNC_SUCCESS,
	NOMAD_SYNC_FAIL,
	NOMAD_ASYNC_QUEUED_SUCCESS,
	NOMAD_ASYNC_QUEUED_FAIL,
};

struct nomad_remap_status {
	bool use_new_page;
	pte_t old_pteval;
	enum ttu_flags flags;
	struct page *new_page;
};

struct demote_shadow_page_context;
// TODO(lingfeng): when rmmod, how to safely reset the function pointer is not considered yet
struct async_promote_ctrl {
	int initialized;
	/**
	* @brief return the number of pages to be queued, if should not queue, return 0
	*
	*/
	int (*queue_page_fault)(struct page *page, pte_t *ptep, int target_nid);

	int (*link_shadow_page)(struct page *newpage, struct page *oldpage);
	struct page *(*demote_shadow_page_find)(
		struct page *page, struct demote_shadow_page_context *contxt);
	bool (*demote_shadow_page_breakup)(
		struct page *page, struct demote_shadow_page_context *contxt);
	struct page *(*release_shadow_page)(struct page *page, void *private,
					    bool in_fault);
	unsigned long (*reclaim_page)(int nid, int nr_to_reclaim);
};

void async_mod_glob_init_set(void);
bool async_mod_glob_inited(void);

extern bool nomad_try_to_migrate(struct page *, enum ttu_flags flags,
				 struct demote_shadow_page_context *context);
extern bool nomad_try_to_remap(struct page *page,
			       struct nomad_remap_status *arg);

extern struct async_promote_ctrl async_mod_glob_ctrl;

#define _PAGE_BIT_ORIGINALLY_WRITABLE 52 /* used for non-exclusive mapping */
#define _PAGE_ORIGINALLY_WRITABLE (_AT(u64, 1) << _PAGE_BIT_ORIGINALLY_WRITABLE)

static inline int pte_orig_writable(pte_t pte)
{
	return (native_pte_val(pte) & _PAGE_ORIGINALLY_WRITABLE) != 0;
}

static inline pte_t pte_mk_orig_writable(pte_t pte)
{
	return pte_set_flags(pte, _PAGE_ORIGINALLY_WRITABLE);
}

static inline pte_t pte_clear_orig_writable(pte_t pte)
{
	return pte_clear_flags(pte, _PAGE_ORIGINALLY_WRITABLE);
}

// used for shadow page demotion context
struct demote_shadow_page_context {
	// should we use shadow page? 1 for yes, 0 for no
	int use_shadow_page;
	// If we find one, increment this number by one. If shadow page is
	// destroyed, then decrement by one. This is used for checking correctness.
	int shadow_page_ref_num;
	int traversed_mapping_num;
	void *flags;
	// Sometimes pte may be write protected eventhough
	// the VMA is writable (not set by our solution). For this
	// we record this situation, and recover mapped page PTE to writable when it
	// gets demoted.
	// Potential cases where shadow relation gets broken:
	// 1. Writing to the page, this page may be forked, we break the relation when
	// a WP fault happens
	// 2. Demote a page, in this case we no longer need the shadow relation.
	// Break it, and restore the PTE.
	// 3. Free a page in userspace. Since shadow page goes with the page
	// rather than the PTE, we break this relation when the page ref count gets 0.
	bool was_writable_before_shadowed;
	bool made_writable;
	struct page *shadow_page;
	// this is the page to be discarded, in the remapping phase of demotion
	struct page *old_page;
};

struct nomad_context {
	uint64_t transactional_migrate_success_nr;
	uint64_t transactional_migrate_fail_nr;
};

#endif
