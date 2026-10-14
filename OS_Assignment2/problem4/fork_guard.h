/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FORK_GUARD_H
#define _LINUX_FORK_GUARD_H

#include <linux/types.h>

struct task_struct;

/*
 * Called at the top of kernel_clone(). Returns 0 to allow the fork or a
 * negative errno (-EAGAIN) to refuse it. Runs before, and independently
 * of, any tier logic.
 */
int fork_guard_check(unsigned long clone_flags);

/*
 * Called from sched_fork(). Returns true and sets *nice when the owner of
 * @p has a non-default (GOLD/BRONZE) tier.
 */
bool fork_guard_tier_nice(struct task_struct *p, int *nice);

#endif /* _LINUX_FORK_GUARD_H */
