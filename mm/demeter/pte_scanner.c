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
	SCAN_INTERVAL_MS_MAX = 3600 * 1000
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
	unsigned long pages_scanned = 0, vm_flags = 0;

	/* Take the LRU lock for this lruvec */
	spin_lock_irq(&lruvec->lru_lock);

	list_for_each_entry_safe (page, next, src, lru) {
		/* Skip if page is being freed or is compound tail */
		if (!PageLRU(page) || PageUnevictable(page) ||
		    !page_evictable(page))
			continue;

		/* Use kernel's page_referenced to examine PTE.A bits and trigger TLB flushes */
		(*referenced) += page_referenced(page, false, memcg, &vm_flags);
		pages_scanned++;

		// /* Don't scan too many pages at once to avoid long lock hold times */
		// if (pages_scanned >= 512)
		// 	break;
	}

	spin_unlock_irq(&lruvec->lru_lock);

	return pages_scanned;
}

static unsigned long scan_zone(struct zone *zone, unsigned long *referenced)
{
	unsigned long pages_scanned = 0;
	struct mem_cgroup *memcg;

	for_each_mem_cgroup (memcg) {
		struct lruvec *lruvec =
			mem_cgroup_lruvec(memcg, zone->zone_pgdat);
		enum lru_list lru;
		for_each_evictable_lru (lru) {
			pages_scanned +=
				scan_lru_list(lruvec, lru, memcg, referenced);
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
