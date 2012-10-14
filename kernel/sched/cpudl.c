/*
 *  kernel/sched/cpudl.c
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

#include <linux/sched.h>
#include <linux/types.h>
#include <asm/barrier.h>
#include <linux/spinlock.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/cpumask.h>

#include "cpudl.h"

/* cache not valid */
#define NO_CACHED_CPU		-1
/* no cpu with deadline task */
#define NO_CPU_DL		-2
/* not valid dl */
#define NO_CACHED_DL		0

static inline void update_cache_slow(struct cpudl *cp)
{
	int best_cpu = NO_CPU_DL;
	u64 best_dl = NO_CACHED_DL;
	u64 current_dl;
	int i;
	
	//for(i = 0; i < NR_CPUS; i++) {
	if(!cpumask_full(cp->free_cpus))
		for_each_cpu_not(i, cp->free_cpus) {
			current_dl = (u64)atomic64_read(&cp->current_dl[i].dl);
			if(current_dl == NO_CACHED_DL)
				continue;
			if(best_dl == NO_CACHED_DL ||
				cp->cmp_dl(best_dl, current_dl)) {
				best_dl = current_dl;
				best_cpu = i;
			}
		}

	smp_wmb();
	atomic_set(&cp->cached_cpu, best_cpu);
}

/*
 * cpudl_find - find the best CPU in the system
 * @cp: the cpudl context
 * @dlo_mask: mask of overloaded runqueues in the 
 * root domain (used only for push operation)
 * @p: the task
 * @later_mask: a mask to fill in with the selected 
 * CPUs (or NULL)
 *
 * Returns: int - best CPU to/from migrate
 * the task
 */
int cpudl_find(struct cpudl *cp, struct cpumask *dlo_mask,
		struct task_struct *p, struct cpumask *later_mask)
{
	int now_cached_cpu = NO_CACHED_CPU;
	u64 now_cached_dl;
	unsigned long flags;
	int best_cpu = -1;
	const struct sched_dl_entity *dl_se;

	if (later_mask && cpumask_and(later_mask, cp->free_cpus,
			&p->cpus_allowed) && cpumask_and(later_mask,
			later_mask, cpu_active_mask))
		return cpumask_any(later_mask);

	//while(true) {
		now_cached_cpu = atomic_read(&cp->cached_cpu);
		/*
		 * there are no CPUs with dl
		 * tasks enqueued
		 */
		if(now_cached_cpu == NO_CPU_DL || now_cached_cpu == NO_CACHED_CPU)
			return -1;
		/*
		 * cache need to be updated
		 * through the slow-path
		 */
		/*
		if(now_cached_cpu == NO_CACHED_CPU) {
			if(!raw_spin_trylock_irqsave(&cp->lock, flags)) {
				update_cache_slow(cp);
				raw_spin_unlock_irqrestore(&cp->lock, flags);
			}
			continue;
		}
		break;
		*/	
	//}

	/* 
	 * cpudl_find is called on behalf
	 * of a pull, so we don't care about
	 * cp->current_dl[now_cached_cpu] value
	 */
	if(!p)
		return now_cached_cpu;

	/*
	 * if cpudl_find is called on behalf of
	 * a push we must check the cpus_allowed
	 * mask and the deadline
	 *
	 * A read barrier is needed,
	 * otherwise we may see 
	 * cp->cached_cpu updated
	 * with an old value in
	 * cp->current_dl
	 */
	smp_rmb();
	now_cached_dl = (u64)atomic64_read(&cp->current_dl[now_cached_cpu].dl);
	/*
	 * a parallel operation may have
	 * changed the deadline value of
	 * now_cached_cpu
	 */ 
	if(now_cached_dl == NO_CACHED_DL)
		return -1;
	
	dl_se = &p->dl;
	if(cpumask_test_cpu(now_cached_cpu, &p->cpus_allowed) && 
		cp->cmp_dl(dl_se->deadline, now_cached_dl)) {
		best_cpu = now_cached_cpu;
		if(later_mask)
			cpumask_set_cpu(best_cpu, later_mask);
	}

	return best_cpu;
}

/*
 * cpudl_set - update the cpudl skiplist
 * @cp: the cpudl skiplist context
 * @cpu: the target cpu
 * @dl: the new earliest deadline for this cpu
 *
 * Notes: assumes cpu_rq(cpu)->lock is locked
 *
 * Returns: (void)
 */
void cpudl_set(struct cpudl *cp, int cpu, u64 dl, int is_valid)
{
	int now_cached_cpu;
	u64 now_cached_dl;
	bool updated = false;
	unsigned long flags;

	/*
	 * if is_valid is set we may have
	 * to update the cached CPU
	 */
	if(is_valid) {
		cpumask_clear_cpu(cpu, cp->free_cpus);
		atomic64_set(&cp->current_dl[cpu].dl, dl);
		while(1) {
			now_cached_cpu = atomic_read(&cp->cached_cpu);
			if(now_cached_cpu != NO_CACHED_CPU && 
				(now_cached_cpu != cpu || updated)) {
				smp_rmb();
				now_cached_dl = (u64)atomic64_read(&cp->current_dl[now_cached_cpu].dl);
			} else {
				if(!raw_spin_trylock_irqsave(&cp->lock, flags)) {
					update_cache_slow(cp);
					raw_spin_unlock_irqrestore(&cp->lock, flags);
					updated = true;
				}
				continue;
			}
				
			if((now_cached_cpu != NO_CPU_DL &&
				now_cached_dl != NO_CACHED_DL &&
				cp->cmp_dl(dl, now_cached_dl)) ||
				atomic_cmpxchg(&cp->cached_cpu, now_cached_cpu, cpu) == now_cached_cpu)
				break;
		}
	} else {
		cpumask_set_cpu(cpu, cp->free_cpus);
		atomic64_set(&cp->current_dl[cpu].dl, NO_CACHED_DL);
		/*
		 * if is_valid is clear we may have
		 * to clear the cached CPU
		 */
		while(1) {
			now_cached_cpu = atomic_read(&cp->cached_cpu);
			if(now_cached_cpu == cpu &&
				atomic_cmpxchg(&cp->cached_cpu, now_cached_cpu, NO_CACHED_CPU) != now_cached_cpu)
				continue;
			if(now_cached_cpu == NO_CACHED_CPU) {
				if(!raw_spin_trylock_irqsave(&cp->lock, flags)) {
					update_cache_slow(cp);
					raw_spin_unlock_irqrestore(&cp->lock, flags);
				}
				/*
				 * here we doesn't have
				 * to wait for the cache to
				 * be valid, so we can
				 * exit immediately
				 */
			}
			break;
		}
	}
}

/*
 * cpudl_init - initialize the cpudl structure
 * @cp: the cpudl skiplist context
 * @cmp_dl: function used to order deadlines inside structure
 */
int cpudl_init(struct cpudl *cp, bool (*cmp_dl)(u64 a, u64 b))
{
	int i;

	raw_spin_lock_init(&cp->lock);
	
	atomic_set(&cp->cached_cpu, NO_CACHED_CPU);
	for(i = 0; i < NR_CPUS; i++)
		atomic64_set(&cp->current_dl[i].dl, NO_CACHED_DL);
	
	cp->cmp_dl = cmp_dl;

	if(!alloc_cpumask_var(&cp->free_cpus, GFP_KERNEL))
		return -ENOMEM;
	cpumask_setall(cp->free_cpus);

	return 0;
}

/*
 * cpudl_cleanup - clean up the cpudl structure
 * @cp: the cpudl skiplist context
 */
void cpudl_cleanup(struct cpudl *cp)
{
	free_cpumask_var(cp->free_cpus);
}
