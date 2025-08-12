#include <linux/memcontrol.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/swap.h>
#include <linux/rmap.h>
#include <linux/pagemap.h>
#include <linux/page-flags.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/numa.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include "../internal.h"

#define MODULE_NAME "pte_scanner"
#define PROC_ENTRY "pte_scan_stats"
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Periodic PTE.A bit scanner using kernel thread");
MODULE_VERSION("3.0");

static struct proc_dir_entry *proc_entry;
static struct task_struct *scanner_thread;
enum {
	SCAN_INTERVAL_MS_DEFAULT = 100,
	SCAN_INTERVAL_MS_MIN = 10,
	SCAN_INTERVAL_MS_MAX = 3600 * 1000,
	ISOLATE_BATCH_SIZE = 512,
};
static unsigned int scan_interval = SCAN_INTERVAL_MS_DEFAULT;
module_param(scan_interval, uint, 0644);
MODULE_PARM_DESC(scan_interval, "Scan interval in ms (default: 100)");
static unsigned scan_interval_clamp(unsigned interval)
{
	if (interval < SCAN_INTERVAL_MS_MIN)
		return SCAN_INTERVAL_MS_MIN;
	if (interval > SCAN_INTERVAL_MS_MAX)
		return SCAN_INTERVAL_MS_MAX;
	return interval;
}

#define for_each_mem_cgroup(iter)                                              \
	for (iter = mem_cgroup_iter(NULL, NULL, NULL); iter != NULL;           \
	     iter = mem_cgroup_iter(NULL, iter, NULL))

struct scan_stats {
	unsigned long total_pages_scanned;
	unsigned long referenced_pages;
	unsigned long scan_time_ns;
	int numa_nodes_scanned;
	unsigned long scan_count;
};

static struct scan_stats cumulative_stats;
static struct scan_stats last_scan_stats;
static DEFINE_MUTEX(stats_mutex);

static unsigned long scan_lru_list(struct lruvec *lruvec, enum lru_list lru,
				   struct mem_cgroup *memcg,
				   unsigned long *referenced)
{
	struct list_head *src = &lruvec->lists[lru];
	struct page *page, *next;
	unsigned long pages_scanned = 0, vm_flags = 0, flags, lruvec_flags = 0;

	/* Take the LRU lock for this lruvec */
	spin_lock_irqsave(&lruvec->lru_lock, flags);
	// relock_page_lruvec_irqsave(page, lruvec, &lruvec_flags);

	list_for_each_entry_safe (page, next, src, lru) {
		/* Skip if page is being freed or is compound tail */
		if (!PageLRU(page) || PageUnevictable(page) ||
		    !page_evictable(page))
			continue;

		get_page(page);
		/* Use kernel's page_referenced to examine PTE.A bits and trigger TLB flushes */
		(*referenced) += page_referenced(page, false, memcg, &vm_flags);
		pages_scanned++;
		put_page(page);

		// /* Don't scan too many pages at once to avoid long lock hold times */
		// if (pages_scanned >= 512)
		// 	break;
	}

	spin_unlock_irqrestore(&lruvec->lru_lock, flags);

	return pages_scanned;
}

extern unsigned long rotate_lru_list(struct lruvec *lruvec, enum lru_list lru,
				     struct mem_cgroup *memcg,
				     unsigned long *referenced);
#if 0
// This function uses structures privately defined in mm/vmscan.c
unsigned long rotate_lru_list(struct lruvec *lruvec, enum lru_list lru,
			      struct mem_cgroup *memcg,
			      unsigned long *referenced)
{
	enum {
		ISOLATE_BATCH_SIZE = 512,
	};
	// isolate_lru_pages uses: reclaim_idx may_unmap order
	struct scan_control control = {
		// Fields used by isolate_lru_pages
		.reclaim_idx = ZONE_NORMAL,
		.may_unmap = 1,
		// Use 0 as its the fallback value used by kswapd
		.order = 0,
		// Others
		.target_mem_cgroup = memcg,
	}, *sc = &control;
	unsigned long total_scanned = 0;

	for (unsigned long nr_scanned = 0, nr_taken = 0;
	     !list_empty(&lruvec->lists[lru]);) {
		LIST_HEAD(page_list);
		// Isolate
		{
			unsigned long flags = 0;
			spin_lock_irqsave(&lruvec->lru_lock, flags);
			nr_taken = isolate_lru_pages(ISOLATE_BATCH_SIZE, lruvec,
						     &page_list, &nr_scanned,
						     sc, lru);
			// We behave like kswapd so borrow its stastics PGSCAN_KSWAPD
			enum vm_event_item item = PGSCAN_KSWAPD;
			if (!cgroup_reclaim(sc))
				__count_vm_events(item, nr_scanned);
			__count_memcg_events(lruvec_memcg(lruvec), item,
					     nr_scanned);
			// We only scan anon pages, but try to be safe anyway
			__count_vm_events(PGSCAN_ANON + is_file_lru(lru),
					  nr_scanned);
			spin_unlock_irqrestore(&lruvec->lru_lock, flags);
		}

		// Scan
		{
			struct page *page, *next;
			list_for_each_entry_safe (page, next, &page_list, lru) {
				unsigned long vm_flags = 0;
				// Use kernel's page_referenced to examine PTE.A bits and trigger TLB flushes
				(*referenced) += page_referenced(
					page, false, memcg, &vm_flags);
				++total_scanned;
				cond_resched();
			}
		}

		// Putback
		{
			unsigned long flags = 0;
			spin_lock_irqsave(&lruvec->lru_lock, flags);
			move_pages_to_lru(lruvec, &page_list);
			__mod_node_page_state(
				lruvec_pgdat(lruvec),
				NR_ISOLATED_ANON + is_file_lru(lru), -nr_taken);
			// We do not reclaim pages, so we do not need to update PGSTEAL_KSWAPD counters
			spin_unlock_irqrestore(&lruvec->lru_lock, flags);
		}

		// We cannot get a full batch of pages, so stop scanning and try again later
		// || nr_taken != nr_scanned
		if (nr_scanned < ISOLATE_BATCH_SIZE)
			break;

		cond_resched();
	}

	return total_scanned;
}
EXPORT_SYMBOL(rotate_lru_list);
#endif

static unsigned long scan_zone(struct zone *zone, unsigned long *referenced)
{
	unsigned long pages_scanned = 0;
	struct mem_cgroup *memcg;

	for_each_mem_cgroup (memcg) {
		struct lruvec *lruvec =
			mem_cgroup_lruvec(memcg, zone->zone_pgdat);
		enum lru_list lru;
		for_each_evictable_lru (lru) {
			if (is_file_lru(lru))
				continue;
			// pages_scanned +=
			// 	scan_lru_list(lruvec, lru, memcg, referenced);
			pages_scanned +=
				rotate_lru_list(lruvec, lru, memcg, referenced);
			/* Yield CPU periodically */
			if (need_resched()) {
				cond_resched();
			}
		}
	}

	return pages_scanned;
}

static void scan_nodes(void)
{
	int nid;
	unsigned long scanned = 0, referenced = 0;
	int nodes_processed = 0;
	ktime_t start_time = ktime_get();

	/* Reset per-scan statistics */
	memset(&last_scan_stats, 0, sizeof(last_scan_stats));

	/* Iterate through all NUMA nodes */
	for_each_node_state (nid, N_NORMAL_MEMORY) {
		pg_data_t *pgdat = NODE_DATA(nid);
		if (!pgdat)
			continue;
		struct zone *zone = &pgdat->node_zones[ZONE_NORMAL];
		if (!populated_zone(zone))
			continue;
		scanned += scan_zone(zone, &referenced);
		nodes_processed++;
		if (need_resched()) {
			cond_resched();
		}
	}

	unsigned long elapsed_ns =
		ktime_to_ns(ktime_sub(ktime_get(), start_time));
	last_scan_stats = (struct scan_stats){
		.total_pages_scanned = scanned,
		.referenced_pages = referenced,
		.scan_time_ns = elapsed_ns,
		.numa_nodes_scanned = nodes_processed,
		.scan_count = 1,
	};

	/* Update cumulative statistics */
	mutex_lock(&stats_mutex);
	cumulative_stats = (struct scan_stats){
		.total_pages_scanned =
			cumulative_stats.total_pages_scanned + scanned,
		.referenced_pages =
			cumulative_stats.referenced_pages + referenced,
		.scan_time_ns = cumulative_stats.scan_time_ns +
				last_scan_stats.scan_time_ns,
		.numa_nodes_scanned =
			cumulative_stats.numa_nodes_scanned + nodes_processed,
		.scan_count = cumulative_stats.scan_count + 1,
	};
	mutex_unlock(&stats_mutex);

	/* Output statistics to dmesg */
	pr_info("PTE.A Scan #%lu: %lu pages scanned, %lu referenced across %d nodes in %lu ns (%lu ms)\n",
		cumulative_stats.scan_count,
		last_scan_stats.total_pages_scanned,
		last_scan_stats.referenced_pages,
		last_scan_stats.numa_nodes_scanned,
		last_scan_stats.scan_time_ns,
		last_scan_stats.scan_time_ns / 1000000);

	if (cumulative_stats.scan_count % 10 == 0) {
		pr_info("PTE.A Scanner: Completed %lu scans, avg scan time: %lu ns, total pages: %lu\n",
			cumulative_stats.scan_count,
			cumulative_stats.scan_time_ns /
				cumulative_stats.scan_count,
			cumulative_stats.total_pages_scanned);
	}
}

static int scanner_thread_fn(void *data)
{
	pr_info("PTE.A scanner thread started, scanning every %u ms\n",
		scan_interval);

	while (!kthread_should_stop()) {
		scan_nodes();
		schedule_timeout_interruptible(msecs_to_jiffies(scan_interval));
	}

	pr_info("PTE.A scanner thread exiting after %lu scans\n",
		cumulative_stats.scan_count);

	return 0;
}

static ssize_t proc_read(struct file *file, char __user *buffer, size_t count,
			 loff_t *pos)
{
	if (*pos > 0)
		return 0;

	char *buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&stats_mutex);

	int len = snprintf(
		buf, PAGE_SIZE,
		"Periodic PTE.A Bit Scanner Statistics\n"
		"=====================================\n"
		"Thread Status: %s\n"
		"Scan Interval: %u ms\n"
		"Total Scans Completed: %lu\n"
		"\nLast Scan Results:\n"
		"  Duration: %lu ns (%lu ms)\n"
		"  NUMA nodes: %d\n"
		"  Pages scanned: %lu\n"
		"  Referenced pages: %lu (%lu%%)\n"
		"\nCumulative Statistics:\n"
		"  Total pages scanned: %lu\n"
		"  Total referenced: %lu\n"
		"  Average scan time: %lu ns\n"
		"  Overall reference rate: %lu%%\n"
		"\nConfiguration:\n"
		"  Module parameter: scan_interval=%u (modifiable via sysfs)\n"
		"  Runtime commands:\n"
		"    echo 'interval=N' > /proc/%s # Set scan interval (ms)\n"
		"    echo 'scan' > /proc/%s       # Trigger immediate scan\n"
		"  Sysfs interface:\n"
		"    echo N > /sys/module/%s/parameters/scan_interval\n",
		scanner_thread ? "Running" : "Stopped", scan_interval,
		cumulative_stats.scan_count, last_scan_stats.scan_time_ns,
		last_scan_stats.scan_time_ns / 1000000,
		last_scan_stats.numa_nodes_scanned,
		last_scan_stats.total_pages_scanned,
		last_scan_stats.referenced_pages,
		last_scan_stats.total_pages_scanned ?
			(last_scan_stats.referenced_pages * 100) /
				last_scan_stats.total_pages_scanned :
			0,
		cumulative_stats.total_pages_scanned,
		cumulative_stats.referenced_pages,
		cumulative_stats.scan_time_ns / cumulative_stats.scan_count,
		cumulative_stats.total_pages_scanned ?
			(cumulative_stats.referenced_pages * 100) /
				cumulative_stats.total_pages_scanned :
			0,
		PROC_ENTRY, PROC_ENTRY, MODULE_NAME);

	mutex_unlock(&stats_mutex);

	if (len < 0) {
		kfree(buf);
		return len;
	}

	ssize_t ret = simple_read_from_buffer(buffer, count, pos, buf, len);
	kfree(buf);

	return ret;
}

static ssize_t proc_write(struct file *file, const char __user *buffer,
			  size_t count, loff_t *pos)
{
	char cmd[32];
	unsigned int new_interval;

	if (count >= sizeof(cmd))
		return -EINVAL;

	if (copy_from_user(cmd, buffer, count))
		return -EFAULT;

	cmd[count] = '\0';

	if (strncmp(cmd, "scan", 4) == 0) {
		pr_info("Triggering immediate PTE.A scan...\n");
		wake_up_process(scanner_thread);
	} else if (sscanf(cmd, "interval=%u", &new_interval) == 1) {
		if (new_interval == scan_interval_clamp(new_interval)) {
			scan_interval = new_interval;
			pr_info("Scan interval updated to %u ms\n",
				scan_interval);
		} else {
			pr_warn("Invalid interval %u, must be between %d and %d ms\n",
				new_interval, SCAN_INTERVAL_MS_MIN,
				SCAN_INTERVAL_MS_MAX);
			return -EINVAL;
		}
	} else {
		return -EINVAL;
	}

	return count;
}

static const struct proc_ops proc_fops = {
	.proc_read = proc_read,
	.proc_write = proc_write,
};

static int __init pte_scanner_init(void)
{
	/* Validate module parameter */
	if (scan_interval != scan_interval_clamp(scan_interval)) {
		pr_err("Invalid scan_interval %u, must be between %d and %d ms\n",
		       scan_interval, SCAN_INTERVAL_MS_MIN,
		       SCAN_INTERVAL_MS_MAX);
		return -EINVAL;
	}

	proc_entry = proc_create(PROC_ENTRY, 0666, NULL, &proc_fops);
	if (!proc_entry) {
		pr_err("Failed to create /proc/%s\n", PROC_ENTRY);
		return -ENOMEM;
	}

	/* Initialize cumulative stats */
	memset(&cumulative_stats, 0, sizeof(cumulative_stats));
	memset(&last_scan_stats, 0, sizeof(last_scan_stats));

	/* Create and start the scanner thread */
	scanner_thread = kthread_run(scanner_thread_fn, NULL, "pte_scanner");
	if (IS_ERR(scanner_thread)) {
		pr_err("Failed to create scanner thread\n");
		proc_remove(proc_entry);
		return PTR_ERR(scanner_thread);
	}

	pr_info("Periodic PTE Scanner module loaded. Thread started, interval=%u ms\n",
		scan_interval);

	return 0;
}

static void __exit pte_scanner_exit(void)
{
	/* Stop the scanner thread */
	if (scanner_thread) {
		kthread_stop(scanner_thread);
		scanner_thread = NULL;
	}

	if (proc_entry) {
		proc_remove(proc_entry);
	}

	pr_info("Periodic PTE Scanner module unloaded. Final stats: %lu scans, %lu total pages\n",
		cumulative_stats.scan_count,
		cumulative_stats.total_pages_scanned);
}

module_init(pte_scanner_init);
module_exit(pte_scanner_exit);
