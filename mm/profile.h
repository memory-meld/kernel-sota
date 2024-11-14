#ifndef _LINUX_PROFILE_H
#define _LINUX_PROFILE_H
#include <linux/sched/cputime.h>
#include <linux/mm.h>
#include <linux/cleanup.h>

static inline u64 task_cputime_adjusted_system(struct task_struct *t)
{
	u64 utime, stime = 0;
	task_cputime_adjusted(t, &utime, &stime);
	return stime;
}
struct vmstat_stopwatch {
	u64 start;
	enum vm_event_item item;
};
static inline struct vmstat_stopwatch
vmstat_stopwatch_new(enum vm_event_item item)
{
	return (struct vmstat_stopwatch){
		.item = item,
		.start = task_cputime_adjusted_system(current),
	};
}
static inline void vmstat_stopwatch_drop(struct vmstat_stopwatch *t)
{
	count_vm_events(t->item,
			task_cputime_adjusted_system(current) - t->start);
}
DEFINE_CLASS(vmstat_stopwatch, struct vmstat_stopwatch,
	     vmstat_stopwatch_drop(&_T), vmstat_stopwatch_new(item),
	     enum vm_event_item item);

#endif // _LINUX_PROFILE_H
