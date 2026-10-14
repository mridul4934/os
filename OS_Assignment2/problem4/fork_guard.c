#include <linux/fork_guard.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sysctl.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/hashtable.h>
#include <linux/delay.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>

static int fg_enable = 1;
static int fg_threshold = 200;
static int fg_window_ms = 1000;
static int fg_throttle_ms = 40;
static int fg_cooldown_s = 10;

static int min_val = 0;
static int max_val = 60000;

struct user_tier_entry {
    uid_t uid;
    char tier[16];
    int nice_val;
    int fork_count;
    unsigned long window_start_jiffies;
    unsigned long last_over_limit_jiffies;
    int stage;
    unsigned long throttle_events;
    unsigned long freeze_events;
    unsigned long reap_events;
    struct hlist_node node;
};

static DEFINE_HASHTABLE(user_map, 4);
static DEFINE_SPINLOCK(tier_lock);

static struct user_tier_entry* get_or_create_user(uid_t uid) {
    struct user_tier_entry *entry;
    hash_for_each_possible(user_map, entry, node, uid) {
        if (entry->uid == uid) return entry;
    }

    entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
    if (!entry) return NULL;

    entry->uid = uid;
    strcpy(entry->tier, "SILVER");
    entry->nice_val = 0;
    entry->fork_count = 0;
    entry->window_start_jiffies = jiffies;
    entry->last_over_limit_jiffies = jiffies;
    entry->stage = 0;
    entry->throttle_events = 0;
    entry->freeze_events = 0;
    entry->reap_events = 0;

    hash_add(user_map, &entry->node, uid);
    return entry;
}

void fork_guard_sched_hook(struct task_struct *p) {
    uid_t uid = task_uid(p).val;
    struct user_tier_entry *entry;
    unsigned long flags;

    if (uid == 0) return;

    spin_lock_irqsave(&tier_lock, flags);
    entry = get_or_create_user(uid);
    if (entry) {
        set_user_nice(p, entry->nice_val);
    }
    spin_unlock_irqrestore(&tier_lock, flags);
}

int fork_guard_check(void) {
    uid_t uid;
    struct user_tier_entry *entry;
    unsigned long now = jiffies;
    unsigned long window_jiffies = msecs_to_jiffies(fg_window_ms);
    unsigned long cooldown_jiffies = msecs_to_jiffies(fg_cooldown_s * 1000);
    unsigned long flags;
    int action_stage = 0;
    int delay_ms = 0;
    struct pid *pgrp;
    int pgid = 0;

    if (!fg_enable) return 0;

    uid = current_uid().val;
    if (uid == 0) return 0;

    spin_lock_irqsave(&tier_lock, flags);
    entry = get_or_create_user(uid);
    if (!entry) {
        spin_unlock_irqrestore(&tier_lock, flags);
        return 0;
    }

    if (time_after(now, entry->last_over_limit_jiffies + cooldown_jiffies)) {
        if (entry->stage > 0) {
            pr_info("fork_guard: de-escalated uid=%d to stage 0\n", uid);
        }
        entry->stage = 0;
    }

    if (time_after(now, entry->window_start_jiffies + window_jiffies)) {
        entry->window_start_jiffies = now;
        entry->fork_count = 0;
    }

    entry->fork_count++;

    if (entry->fork_count > fg_threshold) {
        entry->last_over_limit_jiffies = now;

        if (entry->stage == 0) {
            entry->stage = 1;
            entry->throttle_events++;
            action_stage = 1;
            delay_ms = fg_throttle_ms;
        } else if (entry->stage == 1) {
            entry->stage = 2;
            entry->freeze_events++;
            action_stage = 2;
        } else {
            entry->stage = 3;
            entry->reap_events++;
            action_stage = 3;
            entry->fork_count = 0;
            entry->stage = 0;
        }
    }

    spin_unlock_irqrestore(&tier_lock, flags);

    rcu_read_lock();
    pgrp = task_pgrp(current);
    if (pgrp) pgid = pid_vnr(pgrp);
    rcu_read_unlock();

    if (action_stage == 3) {
        pr_warn("fork_guard: REAP uid=%d pgid=%d (SIGKILL process group)\n", uid, pgid);
        if (pgrp) {
            kill_pgrp(pgrp, SIGCONT, 1);
            kill_pgrp(pgrp, SIGKILL, 1);
        }
        send_sig(SIGKILL, current, 0);
        return -EAGAIN;
    } else if (action_stage == 2) {
        pr_warn("fork_guard: FREEZE uid=%d pgid=%d (SIGSTOP process group)\n", uid, pgid);
        if (pgrp) {
            kill_pgrp(pgrp, SIGSTOP, 1);
        }
        return -EAGAIN;
    } else if (action_stage == 1) {
        pr_warn("fork_guard: THROTTLE uid=%d pgid=%d (>%d forks in %d ms)\n", uid, pgid, fg_threshold, fg_window_ms);
        if (delay_ms > 0) mdelay(delay_ms);
        return -EAGAIN;
    }

    return 0;
}
