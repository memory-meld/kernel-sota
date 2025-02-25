/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VM_EVENT_ITEM_H_INCLUDED
#define VM_EVENT_ITEM_H_INCLUDED

#ifdef CONFIG_ZONE_DMA
#define DMA_ZONE(xx) xx##_DMA,
#else
#define DMA_ZONE(xx)
#endif

#ifdef CONFIG_ZONE_DMA32
#define DMA32_ZONE(xx) xx##_DMA32,
#else
#define DMA32_ZONE(xx)
#endif

#ifdef CONFIG_HIGHMEM
#define HIGHMEM_ZONE(xx) xx##_HIGH,
#else
#define HIGHMEM_ZONE(xx)
#endif

#define FOR_ALL_ZONES(xx) DMA_ZONE(xx) DMA32_ZONE(xx) xx##_NORMAL, HIGHMEM_ZONE(xx) xx##_MOVABLE

enum vm_event_item { PGPGIN, PGPGOUT, PSWPIN, PSWPOUT,
		FOR_ALL_ZONES(PGALLOC),
		FOR_ALL_ZONES(ALLOCSTALL),
		FOR_ALL_ZONES(PGSCAN_SKIP),
		PGFREE, PGACTIVATE, PGDEACTIVATE, PGLAZYFREE,
		PGFAULT, PGMAJFAULT,
		PGLAZYFREED,
		PGREFILL,
		PGREUSE,
		PGSTEAL_KSWAPD,
		PGSTEAL_DIRECT,
		PGDEMOTE_KSWAPD,
		PGDEMOTE_DIRECT,
		PGDEMOTE_FILE,
		PGDEMOTE_ANON,
		PGSCAN_KSWAPD,
		PGSCAN_DIRECT,
		PGSCAN_DIRECT_THROTTLE,
		PGSCAN_ANON,
		PGSCAN_FILE,
		PGSTEAL_ANON,
		PGSTEAL_FILE,
#ifdef CONFIG_NUMA
		PGSCAN_ZONE_RECLAIM_FAILED,
#endif
		PGINODESTEAL, SLABS_SCANNED, KSWAPD_INODESTEAL,
		KSWAPD_LOW_WMARK_HIT_QUICKLY, KSWAPD_HIGH_WMARK_HIT_QUICKLY,
		PAGEOUTRUN, PGROTATED,
		DROP_PAGECACHE, DROP_SLAB,
		OOM_KILL,
#ifdef CONFIG_NUMA_BALANCING
		NUMA_PTE_UPDATES,
		NUMA_HUGE_PTE_UPDATES,
		NUMA_HINT_FAULTS,
		NUMA_HINT_FAULTS_LOCAL,
		NUMA_PAGE_MIGRATE,
		PGPROMOTE_CANDIDATE,		/* candidates get selected for promotion */
		PGPROMOTE_CANDIDATE_DEMOTED,	/* promotion candidate that got demoted earlier */
		PGPROMOTE_CANDIDATE_ANON,	/* promotion candidate that are anon */
		PGPROMOTE_CANDIDATE_FILE,	/* promotion candidate that are file */
		PGPROMOTE_TRIED,		/* tried to migrate via NUMA balancing */
		PGPROMOTE_FILE,			/* successfully promoted file pages  */
		PGPROMOTE_ANON,			/* successfully promoted anon pages  */
#endif
#ifdef CONFIG_MIGRATION
		PGMIGRATE_SUCCESS, PGMIGRATE_FAIL,
		PGMIGRATE_DST_NODE_FULL_FAIL,	/* failed as the target node is full */
		PGMIGRATE_NUMA_ISOLATE_FAIL,	/* failed in isolating numa page */
		PGMIGRATE_NOMEM_FAIL,		/* failed as no memory left */
		PGMIGRATE_REFCOUNT_FAIL,	/* failed in ref count */
		THP_MIGRATION_SUCCESS,
		THP_MIGRATION_FAIL,
		THP_MIGRATION_SPLIT,
#endif
		PEBS_NR_SAMPLED,
		PEBS_NR_SAMPLED_FMEM,
		PEBS_NR_SAMPLED_SMEM,
		//        | Samping                | Classification                           | Migration
		// -------+------------------------+------------------------------------------+--------------------------
		// TPP    | PTEA_SCAN_NS           | LRU_ROTATE_NS - PTEA_SCAN_NS - DEMOTE_NS | DEMOTE_NS + HINT_FAULT_NS
		// Nomad  | PTEA_SCAN_NS           | LRU_ROTATE_NS - PTEA_SCAN_NS - DEMOTE_NS | DEMOTE_NS + HINT_FAULT_NS
		// Memtis | SAMPLING_NS - PTEXT_NS | LRU_ROTATE_NS + PTEXT_NS                 | DEMOTE_NS + PROMOTE_NS
		PTEA_SCAN_NS,	// Only record the rmap overhead
		PTEA_SCANNED,	// Record rmap walked PTEs
		LRU_ROTATE_NS,  // Overhead spent on selecting reclaimation candidates
				// (include PTEA_SCAN_NS and DEMOTE_NS in TPP/Nomad)
		DEMOTE_NS,	// Cost of demoting reclaimation candidates
		HINT_FAULT_NS,	// Software-only overhead spent on NUMA hinting faults
				// (include triggering overhead)
		PROMOTE_NS,	// Memtis promotion candidates migration cost
		SAMPLING_NS,	// Memtis ksamplingd cost (include PTEXT_NS)
		PTEXT_NS,	// Memtis pagetable extension maintance cost
#ifdef CONFIG_COMPACTION
		COMPACTMIGRATE_SCANNED, COMPACTFREE_SCANNED,
		COMPACTISOLATED,
		COMPACTSTALL, COMPACTFAIL, COMPACTSUCCESS,
		KCOMPACTD_WAKE,
		KCOMPACTD_MIGRATE_SCANNED, KCOMPACTD_FREE_SCANNED,
#endif
#ifdef CONFIG_HUGETLB_PAGE
		HTLB_BUDDY_PGALLOC, HTLB_BUDDY_PGALLOC_FAIL,
#endif
#ifdef CONFIG_CMA
		CMA_ALLOC_SUCCESS,
		CMA_ALLOC_FAIL,
#endif
		UNEVICTABLE_PGCULLED,	/* culled to noreclaim list */
		UNEVICTABLE_PGSCANNED,	/* scanned for reclaimability */
		UNEVICTABLE_PGRESCUED,	/* rescued from noreclaim list */
		UNEVICTABLE_PGMLOCKED,
		UNEVICTABLE_PGMUNLOCKED,
		UNEVICTABLE_PGCLEARED,	/* on COW, page truncate */
		UNEVICTABLE_PGSTRANDED,	/* unable to isolate on unlock */
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		THP_FAULT_ALLOC,
		THP_FAULT_FALLBACK,
		THP_FAULT_FALLBACK_CHARGE,
		THP_COLLAPSE_ALLOC,
		THP_COLLAPSE_ALLOC_FAILED,
		THP_FILE_ALLOC,
		THP_FILE_FALLBACK,
		THP_FILE_FALLBACK_CHARGE,
		THP_FILE_MAPPED,
		THP_SPLIT_PAGE,
		THP_SPLIT_PAGE_FAILED,
		THP_DEFERRED_SPLIT_PAGE,
		THP_SPLIT_PMD,
#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
		THP_SPLIT_PUD,
#endif
		THP_ZERO_PAGE_ALLOC,
		THP_ZERO_PAGE_ALLOC_FAILED,
		THP_SWPOUT,
		THP_SWPOUT_FALLBACK,
#endif
#ifdef CONFIG_MEMORY_BALLOON
		BALLOON_INFLATE,
		BALLOON_DEFLATE,
#ifdef CONFIG_BALLOON_COMPACTION
		BALLOON_MIGRATE,
#endif
#endif
#ifdef CONFIG_HTMM
		HTMM_NR_PROMOTED,
		HTMM_NR_DEMOTED,
		HTMM_MISSED_DRAMREAD,
		HTMM_MISSED_NVMREAD,
		HTMM_MISSED_WRITE,
		HTMM_ALLOC_DRAM,
		HTMM_ALLOC_NVM,
#endif
#ifdef CONFIG_DEBUG_TLBFLUSH
		NR_TLB_REMOTE_FLUSH,	/* cpu tried to flush others' tlbs */
		NR_TLB_REMOTE_FLUSH_RECEIVED,/* cpu received ipi for flush */
		NR_TLB_LOCAL_FLUSH_ALL,
		NR_TLB_LOCAL_FLUSH_ONE,
#endif /* CONFIG_DEBUG_TLBFLUSH */
#ifdef CONFIG_DEBUG_VM_VMACACHE
		VMACACHE_FIND_CALLS,
		VMACACHE_FIND_HITS,
#endif
#ifdef CONFIG_SWAP
		SWAP_RA,
		SWAP_RA_HIT,
#endif
#ifdef CONFIG_X86
		DIRECT_MAP_LEVEL2_SPLIT,
		DIRECT_MAP_LEVEL3_SPLIT,
#endif
		NR_VM_EVENT_ITEMS
};

#ifndef CONFIG_TRANSPARENT_HUGEPAGE
#define THP_FILE_ALLOC ({ BUILD_BUG(); 0; })
#define THP_FILE_FALLBACK ({ BUILD_BUG(); 0; })
#define THP_FILE_FALLBACK_CHARGE ({ BUILD_BUG(); 0; })
#define THP_FILE_MAPPED ({ BUILD_BUG(); 0; })
#endif

#endif		/* VM_EVENT_ITEM_H_INCLUDED */
