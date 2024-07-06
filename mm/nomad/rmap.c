#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/rmap.h>
#include <linux/mmu_notifier.h>
#include <linux/hugetlb.h>
#include <linux/sched/numa_balancing.h>

#include <linux/nomad.h>

#include "../internal.h"

extern bool invalid_migration_vma(struct vm_area_struct *vma, void *arg);
extern int page_not_mapped(struct page *page);

static void remap_clean_page(struct page *old, struct page *page,
			     struct page_vma_mapped_walk *pvmw,
			     struct vm_area_struct *vma,
			     struct nomad_remap_status *status)
{
	pte_t new_pteval, old_pteval;
	struct page *new;
	bool use_new_page = true;
	if (PageKsm(page)) {
		new = page;
		pr_info("not expected. [%s]:[%d]", __FILE__, __LINE__);
	} else {
		new = page - old->index + linear_page_index(vma, pvmw->address);
	}
	pr_debug("[%s]:[%d], remap clean page is called.", __FILE__, __LINE__);
#ifdef CONFIG_ARCH_ENABLE_THP_MIGRATION
	/* PMD-mapped THP migration entry */
	if (!pvmw->pte) {
		pr_info("not expected. [%s]:[%d]", __FILE__, __LINE__);
		BUG();
	}
#endif
	old_pteval = status->old_pteval;
	if (pte_dirty(old_pteval)) {
		// go back to previous page`
		use_new_page = false;
		status->use_new_page = false;
	}

	if (use_new_page) {
		get_page(new);
	} else {
		get_page(old);
	}
	if (use_new_page) {
		new_pteval =
			pte_mkold(mk_pte(new, READ_ONCE(vma->vm_page_prot)));
	} else {
		new_pteval =
			pte_mkold(mk_pte(old, READ_ONCE(vma->vm_page_prot)));
	}

	if (pte_swp_soft_dirty(old_pteval))
		new_pteval = pte_mksoft_dirty(new_pteval);

	/*
	* Recheck VMA as permissions can change since migration started
	*/

	if (pte_soft_dirty(old_pteval)) {
		new_pteval = pte_mksoft_dirty(new_pteval);
	}
	if (pte_uffd_wp(old_pteval)) {
		new_pteval = pte_mkuffd_wp(new_pteval);
	}

	if (use_new_page && pte_write(old_pteval)) {
		new_pteval = pte_wrprotect(new_pteval);

		new_pteval = pte_mk_orig_writable(new_pteval);

	} else if (!use_new_page && pte_write(old_pteval)) {
		new_pteval = maybe_mkwrite(new_pteval, vma);
	} else if (!pte_write(old_pteval)) {
		new_pteval = pte_wrprotect(new_pteval);
	} else {
		BUG();
	}

	if (unlikely(is_device_private_page(new))) {
		pr_info("not expected. [%s]:[%d]", __FILE__, __LINE__);
		BUG();
	}

#ifdef CONFIG_HUGETLB_PAGE
	if (PageHuge(new)) {
		pr_info("not expected. [%s]:[%d]", __FILE__, __LINE__);
		BUG();
	} else
#endif
	{
#ifdef CONFIG_NUMA_BALANCING
		if (page_is_demoted(old) && vma_migratable(vma)) {
			bool writable = pte_write(old_pteval);

			new_pteval = pte_modify(new_pteval, PAGE_NONE);
			if (writable)
				new_pteval = pte_mk_savedwrite(new_pteval);
		}
#endif
		set_pte_at(vma->vm_mm, pvmw->address, pvmw->pte, new_pteval);
		// if the page is dirty, we still use the old page
		if (use_new_page) {
			if (PageAnon(new)) {
				page_add_anon_rmap(new, vma, pvmw->address,
						   false);
			} else {
				pr_info("not expected. [%s]:[%d]", __FILE__,
					__LINE__);
				page_add_file_rmap(new, false);
			}
			if (vma->vm_flags & VM_LOCKED &&
			    !PageTransCompound(new))
				mlock_vma_page(new);

			if (PageTransHuge(page) && PageMlocked(page))
				clear_page_mlock(page);
		}
	}

	/* No need to invalidate - it was non-present before */
	update_mmu_cache(vma, pvmw->address, pvmw->pte);
}

// Nomad's original implementation is baed on try_to_unmap. However, in
// v5.14 try_to_migrate has been isolated from try_to_unmap to reduce
// clutter. See commit: cd62734 and a98a2f0

// diff --git a/try_to_unmap.c b/nomad_try_to_unmap.c
// index 90481ec..44c4c30 100644
// --- a/try_to_unmap.c
// +++ b/nomad_try_to_unmap.c
// @@ -1,8 +1,9 @@
// -bool try_to_unmap(struct page *page, enum ttu_flags flags)
// +bool demotion_try_to_unmap(struct page *page, enum ttu_flags flags,
// +                          struct demote_shadow_page_context *context)
//  {
//         struct rmap_walk_control rwc = {
// -               .rmap_one = try_to_unmap_one,
// -               .arg = (void *)flags,
// +               .rmap_one = demotion_try_to_unmap_one,
// +               .arg = (void *)context,
//                 .done = page_not_mapped,
//                 .anon_lock = page_lock_anon_vma_read,
//         };

// diff --git a/try_to_unmap.c b/nomad_try_to_remap.c
// index 90481ec..234a00c 100644
// --- a/try_to_unmap.c
// +++ b/nomad_try_to_remap.c
// @@ -1,8 +1,8 @@
// -bool try_to_unmap(struct page *page, enum ttu_flags flags)
// +bool nomad_try_to_remap(struct page *page, struct nomad_remap_status *arg)
//  {
//         struct rmap_walk_control rwc = {
// -               .rmap_one = try_to_unmap_one,
// -               .arg = (void *)flags,
// +               .rmap_one = nomad_remap_single_ref,
// +               .arg = (void *)arg,
//                 .done = page_not_mapped,
//                 .anon_lock = page_lock_anon_vma_read,
//         };
// @@ -15,11 +15,11 @@ bool try_to_unmap(struct page *page, enum ttu_flags flags)
//          * locking requirements of exec(), migration skips
//          * temporary VMAs until after exec() completes.
//          */
// -       if ((flags & (TTU_MIGRATION|TTU_SPLIT_FREEZE))
// +       if ((arg->flags & (TTU_MIGRATION|TTU_SPLIT_FREEZE))
//             && !PageKsm(page) && PageAnon(page))
//                 rwc.invalid_vma = invalid_migration_vma;
//
// -       if (flags & TTU_RMAP_LOCKED)
// +       if (arg->flags & TTU_RMAP_LOCKED)
//                 rmap_walk_locked(page, &rwc);
//         else
//                 rmap_walk(page, &rwc);

// diff --git a/try_to_unmap_one.c b/nomad_try_to_unmap_one.c
// index fa82cb9..aabc754 100644
// --- a/try_to_unmap_one.c
// +++ b/nomad_try_to_unmap_one.c
// @@ -1,5 +1,6 @@
// -static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
// -                    unsigned long address, void *arg)
// +static bool demotion_try_to_unmap_one(struct page *page,
// +                                     struct vm_area_struct *vma,
// +                                     unsigned long address, void *arg)
//  {
//         struct mm_struct *mm = vma->vm_mm;
//         struct page_vma_mapped_walk pvmw = {
// @@ -11,8 +12,11 @@ static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
//         struct page *subpage;
//         bool ret = true;
//         struct mmu_notifier_range range;
// -       enum ttu_flags flags = (enum ttu_flags)(long)arg;
// +       struct demote_shadow_page_context *contxt =
// +               (struct demote_shadow_page_context *)arg;
// +       enum ttu_flags flags = (enum ttu_flags)(long)(contxt->flags);
//
// +       contxt->traversed_mapping_num += 1;
//         /* munlock has nothing to gain from examining un-locked vmas */
//         if ((flags & TTU_MUNLOCK) && !(vma->vm_flags & VM_LOCKED))
//                 return true;
// @@ -225,6 +229,12 @@ static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
//                                 break;
//                         }
//
// +                       if (pte_orig_writable(pteval)) {
// +                               contxt->was_writable_before_shadowed = true;
// +                       }
// +                       if (pte_write(pteval)) {
// +                               contxt->made_writable = true;
// +                       }
//                         /*
//                          * Store the pfn of the page in a special migration
//                          * pte. do_swap_page() will wait until the migration

// diff --git a/try_to_unmap_one.c b/nomad_try_to_remap_one.c
// index fa82cb9..4e0abb1 100644
// --- a/try_to_unmap_one.c
// +++ b/nomad_try_to_remap_one.c
// @@ -1,5 +1,6 @@
// -static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
// -                    unsigned long address, void *arg)
// +static bool nomad_remap_single_ref(struct page *page,
// +                                       struct vm_area_struct *vma,
// +                                       unsigned long address, void *arg)
//  {
//         struct mm_struct *mm = vma->vm_mm;
//         struct page_vma_mapped_walk pvmw = {
// @@ -11,7 +12,9 @@ static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
//         struct page *subpage;
//         bool ret = true;
//         struct mmu_notifier_range range;
// -       enum ttu_flags flags = (enum ttu_flags)(long)arg;
// +       struct nomad_remap_status *remap_arg =
// +               (struct nomad_remap_status *)arg;
// +       enum ttu_flags flags = remap_arg->flags;
//
//         /* munlock has nothing to gain from examining un-locked vmas */
//         if ((flags & TTU_MUNLOCK) && !(vma->vm_flags & VM_LOCKED))
// @@ -178,7 +181,8 @@ static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
//                 } else {
//                         pteval = ptep_clear_flush(vma, address, pvmw.pte);
//                 }
// -
// +               // this page is unmapped once, ref count decrement by 1
// +               put_page(page);
//                 /* Move the dirty bit to the page. Now the pte is gone. */
//                 if (pte_dirty(pteval))
//                         set_page_dirty(page);
// @@ -215,8 +219,6 @@ static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
//                                                       address + PAGE_SIZE);
//                 } else if (IS_ENABLED(CONFIG_MIGRATION) &&
//                                 (flags & (TTU_MIGRATION|TTU_SPLIT_FREEZE))) {
// -                       swp_entry_t entry;
// -                       pte_t swp_pte;
//
//                         if (arch_unmap_one(mm, vma, address, pteval) < 0) {
//                                 set_pte_at(mm, address, pvmw.pte, pteval);
// @@ -224,24 +226,29 @@ static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
//                                 page_vma_mapped_walk_done(&pvmw);
//                                 break;
//                         }
// -
// -                       /*
// -                        * Store the pfn of the page in a special migration
// -                        * pte. do_swap_page() will wait until the migration
// -                        * pte is removed and then restart fault handling.
// -                        */
// -                       entry = make_migration_entry(subpage,
// -                                       pte_write(pteval));
// -                       swp_pte = swp_entry_to_pte(entry);
// -                       if (pte_soft_dirty(pteval))
// -                               swp_pte = pte_swp_mksoft_dirty(swp_pte);
// -                       if (pte_uffd_wp(pteval))
// -                               swp_pte = pte_swp_mkuffd_wp(swp_pte);
// -                       set_pte_at(mm, address, pvmw.pte, swp_pte);
// -                       /*
// -                        * No need to invalidate here it will synchronize on
// -                        * against the special swap migration pte.
// -                        */
// +                       remap_arg->old_pteval = pteval;
// +                       remap_clean_page(pvmw.page, remap_arg->new_page, &pvmw,
// +                                        vma, remap_arg);
// +                       // /*
// +                       //  * Store the pfn of the page in a special migration
// +                       //  * pte. do_swap_page() will wait until the migration
// +                       //  * pte is removed and then restart fault handling.
// +                       //  */
// +                       // entry = make_migration_entry(subpage,
// +                       //              pte_write(pteval));
// +                       // swp_pte = swp_entry_to_pte(entry);
// +                       // if (pte_soft_dirty(pteval)){
// +                       //      swp_pte = pte_swp_mksoft_dirty(swp_pte);
// +                       //      // the page has been modified during the page copy
// +                       //      remap_arg->unmapped_clean_page = false;
// +                       // }
// +                       // if (pte_uffd_wp(pteval))
// +                       //      swp_pte = pte_swp_mkuffd_wp(swp_pte);
// +                       // set_pte_at(mm, address, pvmw.pte, swp_pte);
// +                       // /*
// +                       //  * No need to invalidate here it will synchronize on
// +                       //  * against the special swap migration pte.
// +                       //  */
//                 } else if (PageAnon(page)) {
//                         swp_entry_t entry = { .val = page_private(subpage) };
//                         pte_t swp_pte;
// @@ -330,8 +337,10 @@ discard:
//                  *
//                  * See Documentation/vm/mmu_notifier.rst
//                  */
// -               page_remove_rmap(subpage, PageHuge(page));
// -               put_page(page);
// +               if (remap_arg->use_new_page) {
// +                       // if not clean, we will use the original page
// +                       page_remove_rmap(subpage, PageHuge(page));
// +               }
//         }
//
//         mmu_notifier_invalidate_range_end(&range);

// diff --git a/try_to_unmap_one.c b/try_to_migrate_one.c
// index fa82cb9..1279d80 100644
// --- a/try_to_unmap_one.c
// +++ b/try_to_migrate_one.c
// @@ -1,4 +1,4 @@
// -static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
// +static bool try_to_migrate_one(struct page *page, struct vm_area_struct *vma,
//                      unsigned long address, void *arg)
//  {
//         struct mm_struct *mm = vma->vm_mm;
// @@ -13,18 +13,21 @@ static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
//         struct mmu_notifier_range range;
//         enum ttu_flags flags = (enum ttu_flags)(long)arg;
//
// -       /* munlock has nothing to gain from examining un-locked vmas */
// -       if ((flags & TTU_MUNLOCK) && !(vma->vm_flags & VM_LOCKED))
// -               return true;
// -
// -       if (IS_ENABLED(CONFIG_MIGRATION) && (flags & TTU_MIGRATION) &&
// -           is_zone_device_page(page) && !is_device_private_page(page))
// -               return true;
// +       /*
// +        * When racing against e.g. zap_pte_range() on another cpu,
// +        * in between its ptep_get_and_clear_full() and page_remove_rmap(),
// +        * try_to_migrate() may return before page_mapped() has become false,
// +        * if page table locking is skipped: use TTU_SYNC to wait for that.
// +        */
// +       if (flags & TTU_SYNC)
// +               pvmw.flags = PVMW_SYNC;
//
// -       if (flags & TTU_SPLIT_HUGE_PMD) {
// -               split_huge_pmd_address(vma, address,
// -                               flags & TTU_SPLIT_FREEZE, page);
// -       }
// +       /*
// +        * unmap_page() in mm/huge_memory.c is the only user of migration with
// +        * TTU_SPLIT_HUGE_PMD and it wants to freeze.
// +        */
// +       if (flags & TTU_SPLIT_HUGE_PMD)
// +               split_huge_pmd_address(vma, address, true, page);
//
//         /*
//          * For THP, we have to assume the worse case ie pmd for invalidation.
// @@ -34,9 +37,10 @@ static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
//          * Note that the page can not be free in this function as call of
//          * try_to_unmap() must hold a reference on the page.
//          */
// +       range.end = PageKsm(page) ?
// +                       address + PAGE_SIZE : vma_address_end(page, vma);
//         mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, vma, vma->vm_mm,
// -                               address,
// -                               min(vma->vm_end, address + page_size(page)));
// +                               address, range.end);
//         if (PageHuge(page)) {
//                 /*
//                  * If sharing is possible, start and end will be adjusted
// @@ -50,37 +54,15 @@ static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
//         while (page_vma_mapped_walk(&pvmw)) {
//  #ifdef CONFIG_ARCH_ENABLE_THP_MIGRATION
//                 /* PMD-mapped THP migration entry */
// -               if (!pvmw.pte && (flags & TTU_MIGRATION)) {
// -                       VM_BUG_ON_PAGE(PageHuge(page) || !PageTransCompound(page), page);
// +               if (!pvmw.pte) {
// +                       VM_BUG_ON_PAGE(PageHuge(page) ||
// +                                      !PageTransCompound(page), page);
//
//                         set_pmd_migration_entry(&pvmw, page);
//                         continue;
//                 }
//  #endif
//
// -               /*
// -                * If the page is mlock()d, we cannot swap it out.
// -                * If it's recently referenced (perhaps page_referenced
// -                * skipped over this mm) then we should reactivate it.
// -                */
// -               if (!(flags & TTU_IGNORE_MLOCK)) {
// -                       if (vma->vm_flags & VM_LOCKED) {
// -                               /* PTE-mapped THP are never mlocked */
// -                               if (!PageTransCompound(page)) {
// -                                       /*
// -                                        * Holding pte lock, we do *not* need
// -                                        * mmap_lock here
// -                                        */
// -                                       mlock_vma_page(page);
// -                               }
// -                               ret = false;
// -                               page_vma_mapped_walk_done(&pvmw);
// -                               break;
// -                       }
// -                       if (flags & TTU_MUNLOCK)
// -                               continue;
// -               }
// -
//                 /* Unexpected PMD-mapped THP? */
//                 VM_BUG_ON_PAGE(!pvmw.pte, page);
//
// @@ -121,20 +103,28 @@ static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
//                         }
//                 }
//
// -               if (IS_ENABLED(CONFIG_MIGRATION) &&
// -                   (flags & TTU_MIGRATION) &&
// -                   is_zone_device_page(page)) {
// +               /* Nuke the page table entry. */
// +               flush_cache_page(vma, address, pte_pfn(*pvmw.pte));
// +               pteval = ptep_clear_flush(vma, address, pvmw.pte);
// +
// +               /* Move the dirty bit to the page. Now the pte is gone. */
// +               if (pte_dirty(pteval))
// +                       set_page_dirty(page);
// +
// +               /* Update high watermark before we lower rss */
// +               update_hiwater_rss(mm);
// +
// +               if (is_zone_device_page(page)) {
//                         swp_entry_t entry;
//                         pte_t swp_pte;
//
// -                       pteval = ptep_get_and_clear(mm, pvmw.address, pvmw.pte);
// -
//                         /*
//                          * Store the pfn of the page in a special migration
//                          * pte. do_swap_page() will wait until the migration
//                          * pte is removed and then restart fault handling.
//                          */
// -                       entry = make_migration_entry(page, 0);
// +                       entry = make_readable_migration_entry(
// +                                                       page_to_pfn(page));
//                         swp_pte = swp_entry_to_pte(entry);
//
//                         /*
// @@ -158,35 +148,7 @@ static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
//                          * memory are supported.
//                          */
//                         subpage = page;
// -                       goto discard;
// -               }
// -
// -               /* Nuke the page table entry. */
// -               flush_cache_page(vma, address, pte_pfn(*pvmw.pte));
// -               if (should_defer_flush(mm, flags)) {
// -                       /*
// -                        * We clear the PTE but do not flush so potentially
// -                        * a remote CPU could still be writing to the page.
// -                        * If the entry was previously clean then the
// -                        * architecture must guarantee that a clear->dirty
// -                        * transition on a cached TLB entry is written through
// -                        * and traps if the PTE is unmapped.
// -                        */
// -                       pteval = ptep_get_and_clear(mm, address, pvmw.pte);
// -
// -                       set_tlb_ubc_flush_pending(mm, pte_dirty(pteval));
// -               } else {
// -                       pteval = ptep_clear_flush(vma, address, pvmw.pte);
// -               }
// -
// -               /* Move the dirty bit to the page. Now the pte is gone. */
// -               if (pte_dirty(pteval))
// -                       set_page_dirty(page);
// -
// -               /* Update high watermark before we lower rss */
// -               update_hiwater_rss(mm);
// -
// -               if (PageHWPoison(page) && !(flags & TTU_IGNORE_HWPOISON)) {
// +               } else if (PageHWPoison(page)) {
//                         pteval = swp_entry_to_pte(make_hwpoison_entry(subpage));
//                         if (PageHuge(page)) {
//                                 hugetlb_count_sub(compound_nr(page), mm);
// @@ -213,8 +175,7 @@ static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
//                         /* We have to invalidate as we cleared the pte */
//                         mmu_notifier_invalidate_range(mm, address,
//                                                       address + PAGE_SIZE);
// -               } else if (IS_ENABLED(CONFIG_MIGRATION) &&
// -                               (flags & (TTU_MIGRATION|TTU_SPLIT_FREEZE))) {
// +               } else {
//                         swp_entry_t entry;
//                         pte_t swp_pte;
//
// @@ -230,8 +191,13 @@ static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
//                          * pte. do_swap_page() will wait until the migration
//                          * pte is removed and then restart fault handling.
//                          */
// -                       entry = make_migration_entry(subpage,
// -                                       pte_write(pteval));
// +                       if (pte_write(pteval))
// +                               entry = make_writable_migration_entry(
// +                                                       page_to_pfn(subpage));
// +                       else
// +                               entry = make_readable_migration_entry(
// +                                                       page_to_pfn(subpage));
// +
//                         swp_pte = swp_entry_to_pte(entry);
//                         if (pte_soft_dirty(pteval))
//                                 swp_pte = pte_swp_mksoft_dirty(swp_pte);
// @@ -242,87 +208,8 @@ static bool try_to_unmap_one(struct page *page, struct vm_area_struct *vma,
//                          * No need to invalidate here it will synchronize on
//                          * against the special swap migration pte.
//                          */
// -               } else if (PageAnon(page)) {
// -                       swp_entry_t entry = { .val = page_private(subpage) };
// -                       pte_t swp_pte;
// -                       /*
// -                        * Store the swap location in the pte.
// -                        * See handle_pte_fault() ...
// -                        */
// -                       if (unlikely(PageSwapBacked(page) != PageSwapCache(page))) {
// -                               WARN_ON_ONCE(1);
// -                               ret = false;
// -                               /* We have to invalidate as we cleared the pte */
// -                               mmu_notifier_invalidate_range(mm, address,
// -                                                       address + PAGE_SIZE);
// -                               page_vma_mapped_walk_done(&pvmw);
// -                               break;
// -                       }
// -
// -                       /* MADV_FREE page check */
// -                       if (!PageSwapBacked(page)) {
// -                               if (!PageDirty(page)) {
// -                                       /* Invalidate as we cleared the pte */
// -                                       mmu_notifier_invalidate_range(mm,
// -                                               address, address + PAGE_SIZE);
// -                                       dec_mm_counter(mm, MM_ANONPAGES);
// -                                       goto discard;
// -                               }
// -
// -                               /*
// -                                * If the page was redirtied, it cannot be
// -                                * discarded. Remap the page to page table.
// -                                */
// -                               set_pte_at(mm, address, pvmw.pte, pteval);
// -                               SetPageSwapBacked(page);
// -                               ret = false;
// -                               page_vma_mapped_walk_done(&pvmw);
// -                               break;
// -                       }
// -
// -                       if (swap_duplicate(entry) < 0) {
// -                               set_pte_at(mm, address, pvmw.pte, pteval);
// -                               ret = false;
// -                               page_vma_mapped_walk_done(&pvmw);
// -                               break;
// -                       }
// -                       if (arch_unmap_one(mm, vma, address, pteval) < 0) {
// -                               set_pte_at(mm, address, pvmw.pte, pteval);
// -                               ret = false;
// -                               page_vma_mapped_walk_done(&pvmw);
// -                               break;
// -                       }
// -                       if (list_empty(&mm->mmlist)) {
// -                               spin_lock(&mmlist_lock);
// -                               if (list_empty(&mm->mmlist))
// -                                       list_add(&mm->mmlist, &init_mm.mmlist);
// -                               spin_unlock(&mmlist_lock);
// -                       }
// -                       dec_mm_counter(mm, MM_ANONPAGES);
// -                       inc_mm_counter(mm, MM_SWAPENTS);
// -                       swp_pte = swp_entry_to_pte(entry);
// -                       if (pte_soft_dirty(pteval))
// -                               swp_pte = pte_swp_mksoft_dirty(swp_pte);
// -                       if (pte_uffd_wp(pteval))
// -                               swp_pte = pte_swp_mkuffd_wp(swp_pte);
// -                       set_pte_at(mm, address, pvmw.pte, swp_pte);
// -                       /* Invalidate as we cleared the pte */
// -                       mmu_notifier_invalidate_range(mm, address,
// -                                                     address + PAGE_SIZE);
// -               } else {
// -                       /*
// -                        * This is a locked file-backed page, thus it cannot
// -                        * be removed from the page cache and replaced by a new
// -                        * page before mmu_notifier_invalidate_range_end, so no
// -                        * concurrent thread might update its page table to
// -                        * point at new page while a device still is using this
// -                        * page.
// -                        *
// -                        * See Documentation/vm/mmu_notifier.rst
// -                        */
// -                       dec_mm_counter(mm, mm_counter_file(page));
//                 }
// -discard:
// +
//                 /*
//                  * No need to call mmu_notifier_invalidate_range() it has be
//                  * done above for all cases requiring it to happen under page

/*
 * @arg: enum ttu_flags will be passed to this argument.
 *
 * If TTU_SPLIT_HUGE_PMD is specified any PMD mappings will be split into PTEs
 * containing migration entries.
 */
static bool nomad_try_to_migrate_one(struct page *page,
				     struct vm_area_struct *vma,
				     unsigned long address, void *arg)
{
	struct mm_struct *mm = vma->vm_mm;
	struct page_vma_mapped_walk pvmw = {
		.page = page,
		.vma = vma,
		.address = address,
	};
	pte_t pteval;
	struct page *subpage;
	bool ret = true;
	struct mmu_notifier_range range;
	struct demote_shadow_page_context *contxt =
		(struct demote_shadow_page_context *)arg;
	enum ttu_flags flags = (enum ttu_flags)(long)(contxt->flags);

	contxt->traversed_mapping_num += 1;

	/*
	 * When racing against e.g. zap_pte_range() on another cpu,
	 * in between its ptep_get_and_clear_full() and page_remove_rmap(),
	 * try_to_migrate() may return before page_mapped() has become false,
	 * if page table locking is skipped: use TTU_SYNC to wait for that.
	 */
	if (flags & TTU_SYNC)
		pvmw.flags = PVMW_SYNC;

	/*
	 * unmap_page() in mm/huge_memory.c is the only user of migration with
	 * TTU_SPLIT_HUGE_PMD and it wants to freeze.
	 */
	if (flags & TTU_SPLIT_HUGE_PMD)
		split_huge_pmd_address(vma, address, true, page);

	/*
	 * For THP, we have to assume the worse case ie pmd for invalidation.
	 * For hugetlb, it could be much worse if we need to do pud
	 * invalidation in the case of pmd sharing.
	 *
	 * Note that the page can not be free in this function as call of
	 * try_to_unmap() must hold a reference on the page.
	 */
	range.end = PageKsm(page) ?
			address + PAGE_SIZE : vma_address_end(page, vma);
	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, vma, vma->vm_mm,
				address, range.end);
	if (PageHuge(page)) {
		/*
		 * If sharing is possible, start and end will be adjusted
		 * accordingly.
		 */
		adjust_range_if_pmd_sharing_possible(vma, &range.start,
						     &range.end);
	}
	mmu_notifier_invalidate_range_start(&range);

	while (page_vma_mapped_walk(&pvmw)) {
#ifdef CONFIG_ARCH_ENABLE_THP_MIGRATION
		/* PMD-mapped THP migration entry */
		if (!pvmw.pte) {
			VM_BUG_ON_PAGE(PageHuge(page) ||
				       !PageTransCompound(page), page);

			set_pmd_migration_entry(&pvmw, page);
			continue;
		}
#endif

		/* Unexpected PMD-mapped THP? */
		VM_BUG_ON_PAGE(!pvmw.pte, page);

		subpage = page - page_to_pfn(page) + pte_pfn(*pvmw.pte);
		address = pvmw.address;

		if (PageHuge(page) && !PageAnon(page)) {
			/*
			 * To call huge_pmd_unshare, i_mmap_rwsem must be
			 * held in write mode.  Caller needs to explicitly
			 * do this outside rmap routines.
			 */
			VM_BUG_ON(!(flags & TTU_RMAP_LOCKED));
			if (huge_pmd_unshare(mm, vma, &address, pvmw.pte)) {
				/*
				 * huge_pmd_unshare unmapped an entire PMD
				 * page.  There is no way of knowing exactly
				 * which PMDs may be cached for this mm, so
				 * we must flush them all.  start/end were
				 * already adjusted above to cover this range.
				 */
				flush_cache_range(vma, range.start, range.end);
				flush_tlb_range(vma, range.start, range.end);
				mmu_notifier_invalidate_range(mm, range.start,
							      range.end);

				/*
				 * The ref count of the PMD page was dropped
				 * which is part of the way map counting
				 * is done for shared PMDs.  Return 'true'
				 * here.  When there is no other sharing,
				 * huge_pmd_unshare returns false and we will
				 * unmap the actual page and drop map count
				 * to zero.
				 */
				page_vma_mapped_walk_done(&pvmw);
				break;
			}
		}

		/* Nuke the page table entry. */
		flush_cache_page(vma, address, pte_pfn(*pvmw.pte));
		pteval = ptep_clear_flush(vma, address, pvmw.pte);

		/* Move the dirty bit to the page. Now the pte is gone. */
		if (pte_dirty(pteval))
			set_page_dirty(page);

		/* Update high watermark before we lower rss */
		update_hiwater_rss(mm);

		if (is_zone_device_page(page)) {
			swp_entry_t entry;
			pte_t swp_pte;

			/*
			 * Store the pfn of the page in a special migration
			 * pte. do_swap_page() will wait until the migration
			 * pte is removed and then restart fault handling.
			 */
			entry = make_readable_migration_entry(
							page_to_pfn(page));
			swp_pte = swp_entry_to_pte(entry);

			/*
			 * pteval maps a zone device page and is therefore
			 * a swap pte.
			 */
			if (pte_swp_soft_dirty(pteval))
				swp_pte = pte_swp_mksoft_dirty(swp_pte);
			if (pte_swp_uffd_wp(pteval))
				swp_pte = pte_swp_mkuffd_wp(swp_pte);
			set_pte_at(mm, pvmw.address, pvmw.pte, swp_pte);
			/*
			 * No need to invalidate here it will synchronize on
			 * against the special swap migration pte.
			 *
			 * The assignment to subpage above was computed from a
			 * swap PTE which results in an invalid pointer.
			 * Since only PAGE_SIZE pages can currently be
			 * migrated, just set it to page. This will need to be
			 * changed when hugepage migrations to device private
			 * memory are supported.
			 */
			subpage = page;
		} else if (PageHWPoison(page)) {
			pteval = swp_entry_to_pte(make_hwpoison_entry(subpage));
			if (PageHuge(page)) {
				hugetlb_count_sub(compound_nr(page), mm);
				set_huge_swap_pte_at(mm, address,
						     pvmw.pte, pteval,
						     vma_mmu_pagesize(vma));
			} else {
				dec_mm_counter(mm, mm_counter(page));
				set_pte_at(mm, address, pvmw.pte, pteval);
			}

		} else if (pte_unused(pteval) && !userfaultfd_armed(vma)) {
			/*
			 * The guest indicated that the page content is of no
			 * interest anymore. Simply discard the pte, vmscan
			 * will take care of the rest.
			 * A future reference will then fault in a new zero
			 * page. When userfaultfd is active, we must not drop
			 * this page though, as its main user (postcopy
			 * migration) will not expect userfaults on already
			 * copied pages.
			 */
			dec_mm_counter(mm, mm_counter(page));
			/* We have to invalidate as we cleared the pte */
			mmu_notifier_invalidate_range(mm, address,
						      address + PAGE_SIZE);
		} else {
			swp_entry_t entry;
			pte_t swp_pte;

			if (arch_unmap_one(mm, vma, address, pteval) < 0) {
				set_pte_at(mm, address, pvmw.pte, pteval);
				ret = false;
				page_vma_mapped_walk_done(&pvmw);
				break;
			}

			if (pte_orig_writable(pteval)) {
				contxt->was_writable_before_shadowed = true;
			}
			if (pte_write(pteval)) {
				contxt->made_writable = true;
			}

			/*
			 * Store the pfn of the page in a special migration
			 * pte. do_swap_page() will wait until the migration
			 * pte is removed and then restart fault handling.
			 */
			if (pte_write(pteval))
				entry = make_writable_migration_entry(
							page_to_pfn(subpage));
			else
				entry = make_readable_migration_entry(
							page_to_pfn(subpage));

			swp_pte = swp_entry_to_pte(entry);
			if (pte_soft_dirty(pteval))
				swp_pte = pte_swp_mksoft_dirty(swp_pte);
			if (pte_uffd_wp(pteval))
				swp_pte = pte_swp_mkuffd_wp(swp_pte);
			set_pte_at(mm, address, pvmw.pte, swp_pte);
			/*
			 * No need to invalidate here it will synchronize on
			 * against the special swap migration pte.
			 */
		}

		/*
		 * No need to call mmu_notifier_invalidate_range() it has be
		 * done above for all cases requiring it to happen under page
		 * table lock before mmu_notifier_invalidate_range_end()
		 *
		 * See Documentation/vm/mmu_notifier.rst
		 */
		page_remove_rmap(subpage, PageHuge(page));
		put_page(page);
	}

	mmu_notifier_invalidate_range_end(&range);

	return ret;
}

/**
 * try_to_migrate - try to replace all page table mappings with swap entries
 * @page: the page to replace page table entries for
 * @flags: action and flags
 *
 * Tries to remove all the page table entries which are mapping this page and
 * replace them with special swap entries. Caller must hold the page lock.
 */
bool nomad_try_to_migrate(struct page *page, enum ttu_flags flags,
			  struct demote_shadow_page_context *context)
{
	struct rmap_walk_control rwc = {
		.rmap_one = nomad_try_to_migrate_one,
		.arg = (void *)context,
		.done = page_not_mapped,
		.anon_lock = page_lock_anon_vma_read,
	};

	/*
	 * Migration always ignores mlock and only supports TTU_RMAP_LOCKED and
	 * TTU_SPLIT_HUGE_PMD and TTU_SYNC flags.
	 */
	if (WARN_ON_ONCE(flags & ~(TTU_RMAP_LOCKED | TTU_SPLIT_HUGE_PMD |
					TTU_SYNC)))
		goto out;

	if (is_zone_device_page(page) && !is_device_private_page(page))
		goto out;

	/*
	 * During exec, a temporary VMA is setup and later moved.
	 * The VMA is moved under the anon_vma lock but not the
	 * page tables leading to a race where migration cannot
	 * find the migration ptes. Rather than increasing the
	 * locking requirements of exec(), migration skips
	 * temporary VMAs until after exec() completes.
	 */
	if (!PageKsm(page) && PageAnon(page))
		rwc.invalid_vma = invalid_migration_vma;

	if (flags & TTU_RMAP_LOCKED)
		rmap_walk_locked(page, &rwc);
	else
		rmap_walk(page, &rwc);

out:
	return !page_mapcount(page);
}

// Transit a page that has only one ref count to new page.
// We only transit the page if the page is clean. If dirty,
// we keep the original page.
static bool nomad_try_to_remap_one(struct page *page,
				   struct vm_area_struct *vma,
				   unsigned long address, void *arg)
{
	struct mm_struct *mm = vma->vm_mm;
	struct page_vma_mapped_walk pvmw = {
		.page = page,
		.vma = vma,
		.address = address,
	};
	pte_t pteval;
	struct page *subpage;
	bool ret = true;
	struct mmu_notifier_range range;
	struct nomad_remap_status *remap_arg = (struct nomad_remap_status *)arg;
	enum ttu_flags flags = remap_arg->flags;

	/*
	 * When racing against e.g. zap_pte_range() on another cpu,
	 * in between its ptep_get_and_clear_full() and page_remove_rmap(),
	 * try_to_migrate() may return before page_mapped() has become false,
	 * if page table locking is skipped: use TTU_SYNC to wait for that.
	 */
	if (flags & TTU_SYNC)
		pvmw.flags = PVMW_SYNC;

	/*
	 * unmap_page() in mm/huge_memory.c is the only user of migration with
	 * TTU_SPLIT_HUGE_PMD and it wants to freeze.
	 */
	if (flags & TTU_SPLIT_HUGE_PMD)
		split_huge_pmd_address(vma, address, true, page);

	/*
	 * For THP, we have to assume the worse case ie pmd for invalidation.
	 * For hugetlb, it could be much worse if we need to do pud
	 * invalidation in the case of pmd sharing.
	 *
	 * Note that the page can not be free in this function as call of
	 * try_to_unmap() must hold a reference on the page.
	 */
	range.end = PageKsm(page) ?
			address + PAGE_SIZE : vma_address_end(page, vma);
	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, vma, vma->vm_mm,
				address, range.end);
	if (PageHuge(page)) {
		/*
		 * If sharing is possible, start and end will be adjusted
		 * accordingly.
		 */
		adjust_range_if_pmd_sharing_possible(vma, &range.start,
						     &range.end);
	}
	mmu_notifier_invalidate_range_start(&range);

	while (page_vma_mapped_walk(&pvmw)) {
#ifdef CONFIG_ARCH_ENABLE_THP_MIGRATION
		/* PMD-mapped THP migration entry */
		if (!pvmw.pte) {
			VM_BUG_ON_PAGE(PageHuge(page) ||
				       !PageTransCompound(page), page);

			set_pmd_migration_entry(&pvmw, page);
			continue;
		}
#endif

		/* Unexpected PMD-mapped THP? */
		VM_BUG_ON_PAGE(!pvmw.pte, page);

		subpage = page - page_to_pfn(page) + pte_pfn(*pvmw.pte);
		address = pvmw.address;

		if (PageHuge(page) && !PageAnon(page)) {
			/*
			 * To call huge_pmd_unshare, i_mmap_rwsem must be
			 * held in write mode.  Caller needs to explicitly
			 * do this outside rmap routines.
			 */
			VM_BUG_ON(!(flags & TTU_RMAP_LOCKED));
			if (huge_pmd_unshare(mm, vma, &address, pvmw.pte)) {
				/*
				 * huge_pmd_unshare unmapped an entire PMD
				 * page.  There is no way of knowing exactly
				 * which PMDs may be cached for this mm, so
				 * we must flush them all.  start/end were
				 * already adjusted above to cover this range.
				 */
				flush_cache_range(vma, range.start, range.end);
				flush_tlb_range(vma, range.start, range.end);
				mmu_notifier_invalidate_range(mm, range.start,
							      range.end);

				/*
				 * The ref count of the PMD page was dropped
				 * which is part of the way map counting
				 * is done for shared PMDs.  Return 'true'
				 * here.  When there is no other sharing,
				 * huge_pmd_unshare returns false and we will
				 * unmap the actual page and drop map count
				 * to zero.
				 */
				page_vma_mapped_walk_done(&pvmw);
				break;
			}
		}

		/* Nuke the page table entry. */
		flush_cache_page(vma, address, pte_pfn(*pvmw.pte));
		pteval = ptep_clear_flush(vma, address, pvmw.pte);

		// this page is unmapped once, ref count decrement by 1
		put_page(page);

		/* Move the dirty bit to the page. Now the pte is gone. */
		if (pte_dirty(pteval))
			set_page_dirty(page);

		/* Update high watermark before we lower rss */
		update_hiwater_rss(mm);

		if (is_zone_device_page(page)) {
			swp_entry_t entry;
			pte_t swp_pte;

			/*
			 * Store the pfn of the page in a special migration
			 * pte. do_swap_page() will wait until the migration
			 * pte is removed and then restart fault handling.
			 */
			entry = make_readable_migration_entry(
							page_to_pfn(page));
			swp_pte = swp_entry_to_pte(entry);

			/*
			 * pteval maps a zone device page and is therefore
			 * a swap pte.
			 */
			if (pte_swp_soft_dirty(pteval))
				swp_pte = pte_swp_mksoft_dirty(swp_pte);
			if (pte_swp_uffd_wp(pteval))
				swp_pte = pte_swp_mkuffd_wp(swp_pte);
			set_pte_at(mm, pvmw.address, pvmw.pte, swp_pte);
			/*
			 * No need to invalidate here it will synchronize on
			 * against the special swap migration pte.
			 *
			 * The assignment to subpage above was computed from a
			 * swap PTE which results in an invalid pointer.
			 * Since only PAGE_SIZE pages can currently be
			 * migrated, just set it to page. This will need to be
			 * changed when hugepage migrations to device private
			 * memory are supported.
			 */
			subpage = page;
		} else if (PageHWPoison(page)) {
			pteval = swp_entry_to_pte(make_hwpoison_entry(subpage));
			if (PageHuge(page)) {
				hugetlb_count_sub(compound_nr(page), mm);
				set_huge_swap_pte_at(mm, address,
						     pvmw.pte, pteval,
						     vma_mmu_pagesize(vma));
			} else {
				dec_mm_counter(mm, mm_counter(page));
				set_pte_at(mm, address, pvmw.pte, pteval);
			}

		} else if (pte_unused(pteval) && !userfaultfd_armed(vma)) {
			/*
			 * The guest indicated that the page content is of no
			 * interest anymore. Simply discard the pte, vmscan
			 * will take care of the rest.
			 * A future reference will then fault in a new zero
			 * page. When userfaultfd is active, we must not drop
			 * this page though, as its main user (postcopy
			 * migration) will not expect userfaults on already
			 * copied pages.
			 */
			dec_mm_counter(mm, mm_counter(page));
			/* We have to invalidate as we cleared the pte */
			mmu_notifier_invalidate_range(mm, address,
						      address + PAGE_SIZE);
		} else {
			// swp_entry_t entry;
			// pte_t swp_pte;

			if (arch_unmap_one(mm, vma, address, pteval) < 0) {
				set_pte_at(mm, address, pvmw.pte, pteval);
				ret = false;
				page_vma_mapped_walk_done(&pvmw);
				break;
			}

			// /*
			//  * Store the pfn of the page in a special migration
			//  * pte. do_swap_page() will wait until the migration
			//  * pte is removed and then restart fault handling.
			//  */
			// if (pte_write(pteval))
			// 	entry = make_writable_migration_entry(
			// 				page_to_pfn(subpage));
			// else
			// 	entry = make_readable_migration_entry(
			// 				page_to_pfn(subpage));

			// swp_pte = swp_entry_to_pte(entry);
			// if (pte_soft_dirty(pteval))
			// 	swp_pte = pte_swp_mksoft_dirty(swp_pte);
			// if (pte_uffd_wp(pteval))
			// 	swp_pte = pte_swp_mkuffd_wp(swp_pte);
			// set_pte_at(mm, address, pvmw.pte, swp_pte);
			// /*
			//  * No need to invalidate here it will synchronize on
			//  * against the special swap migration pte.
			//  */

			remap_arg->old_pteval = pteval;
			remap_clean_page(pvmw.page, remap_arg->new_page, &pvmw,
					 vma, remap_arg);
		}

		/*
		 * No need to call mmu_notifier_invalidate_range() it has be
		 * done above for all cases requiring it to happen under page
		 * table lock before mmu_notifier_invalidate_range_end()
		 *
		 * See Documentation/vm/mmu_notifier.rst
		 */
		if (remap_arg->use_new_page) {
			// if not clean, we will use the original page
			page_remove_rmap(subpage, PageHuge(page));
		}
	}

	mmu_notifier_invalidate_range_end(&range);

	return ret;
}

bool nomad_try_to_remap(struct page *page, struct nomad_remap_status *context)
{
	struct rmap_walk_control rwc = {
		.rmap_one = nomad_try_to_remap_one,
		.arg = (void *)context,
		.done = page_not_mapped,
		.anon_lock = page_lock_anon_vma_read,
	};
	enum ttu_flags flags = (enum ttu_flags)(long)(context->flags);

	/*
	 * Migration always ignores mlock and only supports TTU_RMAP_LOCKED and
	 * TTU_SPLIT_HUGE_PMD and TTU_SYNC flags.
	 */
	if (WARN_ON_ONCE(flags & ~(TTU_RMAP_LOCKED | TTU_SPLIT_HUGE_PMD |
					TTU_SYNC)))
		goto out;

	if (is_zone_device_page(page) && !is_device_private_page(page))
		goto out;

	/*
	 * During exec, a temporary VMA is setup and later moved.
	 * The VMA is moved under the anon_vma lock but not the
	 * page tables leading to a race where migration cannot
	 * find the migration ptes. Rather than increasing the
	 * locking requirements of exec(), migration skips
	 * temporary VMAs until after exec() completes.
	 */
	if (!PageKsm(page) && PageAnon(page))
		rwc.invalid_vma = invalid_migration_vma;

	if (flags & TTU_RMAP_LOCKED)
		rmap_walk_locked(page, &rwc);
	else
		rmap_walk(page, &rwc);

out:
	return !page_mapcount(page);
}
