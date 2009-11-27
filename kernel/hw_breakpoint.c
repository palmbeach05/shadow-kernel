/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (C) 2007 Alan Stern
 * Copyright (C) IBM Corporation, 2009
 * Copyright (C) 2009, Frederic Weisbecker <fweisbec@gmail.com>
 */

/*
 * HW_breakpoint: a unified kernel/user-space hardware breakpoint facility,
 * using the CPU's debug registers.
 * This file contains the arch-independent routines.
 */

#include <linux/irqflags.h>
#include <linux/kallsyms.h>
#include <linux/notifier.h>
#include <linux/kprobes.h>
#include <linux/kdebug.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/smp.h>

#include <linux/hw_breakpoint.h>

#include <asm/processor.h>

#ifdef CONFIG_X86
#include <asm/debugreg.h>
#endif

static atomic_t bp_slot;

int reserve_bp_slot(struct perf_event *bp)
{
	if (atomic_inc_return(&bp_slot) == HBP_NUM) {
		atomic_dec(&bp_slot);

		return -ENOSPC;
	}

	return 0;
}

void release_bp_slot(struct perf_event *bp)
{
	atomic_dec(&bp_slot);
}

int __register_perf_hw_breakpoint(struct perf_event *bp)
{
	int ret;

	ret = reserve_bp_slot(bp);
	if (ret)
		return ret;

	if (!bp->attr.disabled)
		ret = arch_validate_hwbkpt_settings(bp, bp->ctx->task);

	return ret;
}

int register_perf_hw_breakpoint(struct perf_event *bp)
{
	bp->callback = perf_bp_event;

	return __register_perf_hw_breakpoint(bp);
}

/**
 * register_user_hw_breakpoint - register a hardware breakpoint for user space
 * @attr: breakpoint attributes
 * @triggered: callback to trigger when we hit the breakpoint
 * @tsk: pointer to 'task_struct' of the process to which the address belongs
 */
struct perf_event *
register_user_hw_breakpoint(struct perf_event_attr *attr,
			    perf_callback_t triggered,
			    struct task_struct *tsk)
{
	return perf_event_create_kernel_counter(attr, -1, tsk->pid, triggered);
}
EXPORT_SYMBOL_GPL(register_user_hw_breakpoint);

/**
 * modify_user_hw_breakpoint - modify a user-space hardware breakpoint
 * @bp: the breakpoint structure to modify
 * @attr: new breakpoint attributes
 * @triggered: callback to trigger when we hit the breakpoint
 * @tsk: pointer to 'task_struct' of the process to which the address belongs
 */
struct perf_event *
modify_user_hw_breakpoint(struct perf_event *bp, struct perf_event_attr *attr,
			  perf_callback_t triggered,
			  struct task_struct *tsk)
{
	/*
	 * FIXME: do it without unregistering
	 * - We don't want to lose our slot
	 * - If the new bp is incorrect, don't lose the older one
	 */
	unregister_hw_breakpoint(bp);

	return perf_event_create_kernel_counter(attr, -1, tsk->pid, triggered);
}
EXPORT_SYMBOL_GPL(modify_user_hw_breakpoint);

/**
 * unregister_hw_breakpoint - unregister a user-space hardware breakpoint
 * @bp: the breakpoint structure to unregister
 */
void unregister_hw_breakpoint(struct perf_event *bp)
{
	if (!bp)
		return;
	perf_event_release_kernel(bp);
}
EXPORT_SYMBOL_GPL(unregister_hw_breakpoint);

/**
 * register_wide_hw_breakpoint - register a wide breakpoint in the kernel
 * @attr: breakpoint attributes
 * @triggered: callback to trigger when we hit the breakpoint
 *
 * @return a set of per_cpu pointers to perf events
 */
struct perf_event **
register_wide_hw_breakpoint(struct perf_event_attr *attr,
			    perf_callback_t triggered)
{
	struct perf_event **cpu_events, **pevent, *bp;
	long err;
	int cpu;

	cpu_events = alloc_percpu(typeof(*cpu_events));
	if (!cpu_events)
		return ERR_PTR(-ENOMEM);

	for_each_possible_cpu(cpu) {
		pevent = per_cpu_ptr(cpu_events, cpu);
		bp = perf_event_create_kernel_counter(attr, cpu, -1, triggered);

		*pevent = bp;

		if (IS_ERR(bp) || !bp) {
			err = PTR_ERR(bp);
			goto fail;
		}
	}

	return cpu_events;

fail:
	for_each_possible_cpu(cpu) {
		pevent = per_cpu_ptr(cpu_events, cpu);
		if (IS_ERR(*pevent) || !*pevent)
			break;
		unregister_hw_breakpoint(*pevent);
	}
	free_percpu(cpu_events);
	/* return the error if any */
	return ERR_PTR(err);
}

/**
 * unregister_wide_hw_breakpoint - unregister a wide breakpoint in the kernel
 * @cpu_events: the per cpu set of events to unregister
 */
void unregister_wide_hw_breakpoint(struct perf_event **cpu_events)
{
	int cpu;
	struct perf_event **pevent;

	for_each_possible_cpu(cpu) {
		pevent = per_cpu_ptr(cpu_events, cpu);
		unregister_hw_breakpoint(*pevent);
	}
	free_percpu(cpu_events);
}


static struct notifier_block hw_breakpoint_exceptions_nb = {
	.notifier_call = hw_breakpoint_exceptions_notify,
	/* we need to be notified first */
	.priority = 0x7fffffff
};

static int __init init_hw_breakpoint(void)
{
	return register_die_notifier(&hw_breakpoint_exceptions_nb);
}
core_initcall(init_hw_breakpoint);


struct pmu perf_ops_bp = {
	.enable		= arch_install_hw_breakpoint,
	.disable	= arch_uninstall_hw_breakpoint,
	.read		= hw_breakpoint_pmu_read,
	.unthrottle	= hw_breakpoint_pmu_unthrottle
};
