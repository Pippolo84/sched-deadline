/*
 *  kernel/sched/cpudl.h
 *
 *  CPU deadlines global management
 *
 *  Author: Fabio Falzoi <fabio.falzoi@alice.it>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; version 2
 *  of the License.
 */

#ifndef _LINUX_CPUDL_H
#define _LINUX_CPUDL_H

#include <linux/cpumask.h>
#include <linux/types.h>

#define CACHE_LINE_SIZE	64

struct curr_dl_item {
	atomic64_t dl; 
	u8 padding[CACHE_LINE_SIZE - sizeof(atomic64_t)];
};

struct cpudl {
	cpumask_var_t free_cpus;

	bool (*cmp_dl)(u64 a, u64 b);

	atomic_t cached_cpu;
	struct curr_dl_item current_dl[NR_CPUS] __attribute__ ((aligned (CACHE_LINE_SIZE)));

	raw_spinlock_t lock;
};

#ifdef CONFIG_SMP
int cpudl_find(struct cpudl *cp, struct cpumask *dlo_mask,
		struct task_struct *p, struct cpumask *later_mask);
void cpudl_set(struct cpudl *cp, int cpu, u64 dl, int is_valid);
int cpudl_init(struct cpudl *cp, bool (*cmp_dl)(u64 a, u64 b));
void cpudl_cleanup(struct cpudl *cp);
#else
#define cpudl_set(cp, cpu, dl) do { } while (0)
#define cpudl_init() do { } while (0)
#endif /* CONFIG_SMP */

#endif /* _LINUX_CPUDL_H */
