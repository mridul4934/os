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
#include <linux/kprobes.h>
#include <linux/delay.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS Assignment Group");
MODULE_DESCRIPTION("Fork Storm Containment and Tiered CPU Scheduling");

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

static void apply_tier_to_user_processes(uid_t target_uid, int nice_val) {
    struct task_struct *g, *p;
    rcu_read_lock();
    for_each_process_thread(g, p) {
        if (task_uid(p).val == target_uid) {
            set_user_nice(p, nice_val);
        }
    }
    rcu_read_unlock();
}

static int kp_pre_handler(struct kprobe *p, struct pt_regs *regs) {
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

    if (entry->nice_val != 0) {
        set_user_nice(current, entry->nice_val);
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
    } else if (action_stage == 2) {
        pr_warn("fork_guard: FREEZE uid=%d pgid=%d (SIGSTOP process group)\n", uid, pgid);
        if (pgrp) {
            kill_pgrp(pgrp, SIGSTOP, 1);
        }
    } else if (action_stage == 1) {
        pr_warn("fork_guard: THROTTLE uid=%d pgid=%d (>%d forks in %d ms)\n", uid, pgid, fg_threshold, fg_window_ms);
        if (delay_ms > 0 && delay_ms <= 50) {
            mdelay(delay_ms);
        }
    }

    return 0;
}

static struct kprobe kp = {
    .symbol_name = "kernel_clone",
    .pre_handler = kp_pre_handler,
};

static int sched_tier_show(struct seq_file *m, void *v) {
    struct user_tier_entry *entry;
    int bkt;
    unsigned long flags;

    seq_printf(m, "UID\tTIER\tNICE\n");
    spin_lock_irqsave(&tier_lock, flags);
    hash_for_each(user_map, bkt, entry, node) {
        seq_printf(m, "%d\t%s\t%d\n", entry->uid, entry->tier, entry->nice_val);
    }
    spin_unlock_irqrestore(&tier_lock, flags);
    return 0;
}

static int sched_tier_open(struct inode *inode, struct file *file) {
    return single_open(file, sched_tier_show, NULL);
}

static ssize_t sched_tier_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    char kbuf[64];
    uid_t uid;
    char tier_str[16];
    struct user_tier_entry *entry, *other;
    int bkt;
    unsigned long flags;
    int nice_val = 0;
    uid_t demote_uid = 0;
    int demote_needed = 0;

    if (!capable(CAP_SYS_ADMIN)) {
        return -EPERM;
    }

    if (count >= sizeof(kbuf)) return -EINVAL;
    if (copy_from_user(kbuf, buf, count)) return -EFAULT;
    kbuf[count] = '\0';

    if (sscanf(kbuf, "SET %u %15s", &uid, tier_str) == 2) {
        if (strcmp(tier_str, "GOLD") != 0 && strcmp(tier_str, "SILVER") != 0 && strcmp(tier_str, "BRONZE") != 0) {
            return -EINVAL;
        }

        spin_lock_irqsave(&tier_lock, flags);
        if (strcmp(tier_str, "GOLD") == 0) {
            hash_for_each(user_map, bkt, other, node) {
                if (strcmp(other->tier, "GOLD") == 0 && other->uid != uid) {
                    strcpy(other->tier, "SILVER");
                    other->nice_val = 0;
                    demote_uid = other->uid;
                    demote_needed = 1;
                }
            }
        }

        entry = get_or_create_user(uid);
        if (entry) {
            strcpy(entry->tier, tier_str);
            if (strcmp(tier_str, "GOLD") == 0) entry->nice_val = -7;
            else if (strcmp(tier_str, "BRONZE") == 0) entry->nice_val = 5;
            else entry->nice_val = 0;
            nice_val = entry->nice_val;
        }
        spin_unlock_irqrestore(&tier_lock, flags);

        if (demote_needed) {
            pr_info("fork_guard: GOLD demotion uid=%d demoted to SILVER nice=0\n", demote_uid);
            apply_tier_to_user_processes(demote_uid, 0);
        }

        pr_info("fork_guard: tier changed uid=%d tier=%s nice=%d\n", uid, tier_str, nice_val);
        apply_tier_to_user_processes(uid, nice_val);

    } else if (strncmp(kbuf, "RESET", 5) == 0) {
        spin_lock_irqsave(&tier_lock, flags);
        hash_for_each(user_map, bkt, entry, node) {
            strcpy(entry->tier, "SILVER");
            entry->nice_val = 0;
        }
        spin_unlock_irqrestore(&tier_lock, flags);

        rcu_read_lock();
        struct task_struct *g, *p;
        for_each_process_thread(g, p) {
            if (task_uid(p).val >= 1000) {
                set_user_nice(p, 0);
            }
        }
        rcu_read_unlock();
        pr_info("fork_guard: reset all users to SILVER\n");
    } else {
        return -EINVAL;
    }

    return count;
}

static const struct proc_ops sched_tier_ops = {
    .proc_open = sched_tier_open,
    .proc_read = seq_read,
    .proc_write = sched_tier_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static int fork_guard_show(struct seq_file *m, void *v) {
    struct user_tier_entry *entry;
    int bkt;
    unsigned long flags;

    seq_printf(m, "enabled=%d threshold=%d window_ms=%d throttle_ms=%d cooldown_s=%d\n",
               fg_enable, fg_threshold, fg_window_ms, fg_throttle_ms, fg_cooldown_s);
    seq_printf(m, "UID\tTIER\tFORKS\tSTAGE\tTHROTTLE\tFREEZE\tREAP\n");
    spin_lock_irqsave(&tier_lock, flags);
    hash_for_each(user_map, bkt, entry, node) {
        seq_printf(m, "%d\t%s\t%d\t%d\t%lu\t\t%lu\t%lu\n",
                   entry->uid, entry->tier, entry->fork_count, entry->stage,
                   entry->throttle_events, entry->freeze_events, entry->reap_events);
    }
    spin_unlock_irqrestore(&tier_lock, flags);
    return 0;
}

static int fork_guard_open(struct inode *inode, struct file *file) {
    return single_open(file, fork_guard_show, NULL);
}

static const struct proc_ops fork_guard_ops = {
    .proc_open = fork_guard_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static struct ctl_table fg_sysctls[] = {
    { .procname = "fg_enable",      .data = &fg_enable,      .maxlen = sizeof(int), .mode = 0644, .proc_handler = proc_dointvec_minmax, .extra1 = &min_val, .extra2 = &max_val },
    { .procname = "fg_threshold",   .data = &fg_threshold,   .maxlen = sizeof(int), .mode = 0644, .proc_handler = proc_dointvec_minmax, .extra1 = &min_val, .extra2 = &max_val },
    { .procname = "fg_window_ms",   .data = &fg_window_ms,   .maxlen = sizeof(int), .mode = 0644, .proc_handler = proc_dointvec_minmax, .extra1 = &min_val, .extra2 = &max_val },
    { .procname = "fg_throttle_ms", .data = &fg_throttle_ms, .maxlen = sizeof(int), .mode = 0644, .proc_handler = proc_dointvec_minmax, .extra1 = &min_val, .extra2 = &max_val },
    { .procname = "fg_cooldown_s",  .data = &fg_cooldown_s,  .maxlen = sizeof(int), .mode = 0644, .proc_handler = proc_dointvec_minmax, .extra1 = &min_val, .extra2 = &max_val }
};

static struct ctl_table_header *fg_sysctl_header;

static int __init forkguard_init(void) {
    int ret;

    fg_sysctl_header = register_sysctl("kernel", fg_sysctls);
    if (!fg_sysctl_header) {
        pr_err("fork_guard: register_sysctl failed\n");
        return -ENOMEM;
    }

    if (!proc_create("sched_tier", 0644, NULL, &sched_tier_ops)) {
        pr_err("fork_guard: failed to create /proc/sched_tier\n");
        unregister_sysctl_table(fg_sysctl_header);
        return -ENOMEM;
    }

    if (!proc_create("fork_guard", 0444, NULL, &fork_guard_ops)) {
        pr_err("fork_guard: failed to create /proc/fork_guard\n");
        remove_proc_entry("sched_tier", NULL);
        unregister_sysctl_table(fg_sysctl_header);
        return -ENOMEM;
    }

    ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("fork_guard: register_kprobe failed: %d\n", ret);
        remove_proc_entry("fork_guard", NULL);
        remove_proc_entry("sched_tier", NULL);
        unregister_sysctl_table(fg_sysctl_header);
        return ret;
    }

    pr_info("fork_guard: initialised (threshold=%d/%dms, gold nice=-7)\n", fg_threshold, fg_window_ms);
    return 0;
}

static void __exit forkguard_exit(void) {
    struct user_tier_entry *entry;
    struct hlist_node *tmp;
    int bkt;
    unsigned long flags;

    unregister_kprobe(&kp);
    if (fg_sysctl_header) {
        unregister_sysctl_table(fg_sysctl_header);
        fg_sysctl_header = NULL;
    }
    remove_proc_entry("sched_tier", NULL);
    remove_proc_entry("fork_guard", NULL);

    spin_lock_irqsave(&tier_lock, flags);
    hash_for_each_safe(user_map, bkt, tmp, entry, node) {
        apply_tier_to_user_processes(entry->uid, 0);
        hash_del(&entry->node);
        kfree(entry);
    }
    spin_unlock_irqrestore(&tier_lock, flags);

    pr_info("fork_guard: unloaded\n");
}

module_init(forkguard_init);
module_exit(forkguard_exit);
