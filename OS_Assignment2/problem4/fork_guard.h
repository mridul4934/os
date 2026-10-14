#ifndef _LINUX_FORK_GUARD_H
#define _LINUX_FORK_GUARD_H

#include <linux/types.h>
#include <linux/sched.h>

int fork_guard_check(void);
void fork_guard_sched_hook(struct task_struct *p);

#endif
