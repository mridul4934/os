# Problem 4 – Fork Storm Containment & Tiered CPU Scheduling

Everything is built into the kernel (Linux 6.8); there is no loadable module.

| File | Goes to | Purpose |
|---|---|---|
| `fork_guard.c` | `kernel/fork_guard.c` | containment ladder, tiers, `/proc`, sysctl |
| `fork_guard.h` | `include/linux/fork_guard.h` | hook prototypes |
| `forkguard-linux-6.8.patch` | apply in kernel tree | all of the above + hooks in `kernel/fork.c` (`kernel_clone()`), `kernel/sched/core.c` (`sched_fork()`), `kernel/Makefile` |
| `tier.c` | userspace | `tier set <user> <gold\|silver\|bronze>`, `tier reset`, `tier show` |

## Build

```sh
cd linux-6.8
patch -p1 < /path/to/problem4/forkguard-linux-6.8.patch
# then the config/build steps from the help doc (LOCALVERSION "-forkguard")
make -j$(nproc) && sudo make modules_install install && sudo update-grub && sudo reboot

gcc -Wall -Wextra -O2 tier.c -o tier && sudo cp tier /usr/local/bin/
```

## Interfaces

* `/proc/sys/kernel/fg_enable` (0/1), `fg_threshold` (forks per window, ≥1), `fg_window_ms`,
  `fg_throttle_ms`, `fg_cooldown_s` – change at runtime with `sysctl -w kernel.fg_threshold=50`.
* `/proc/fork_guard` (read-only): live tunables, then per user: tier, forks in the current
  sliding window, stage, cumulative THROTTLE / FREEZE / REAP counts.
* `/proc/fork_guard_ctl` (root only, mode 0200): `set <uid> <tier>` or `reset`. Used by `tier`.
* dmesg lines are prefixed `fork_guard:`.

## Design (viva notes)

**Escalation ladder.** Fork attempts are counted per user with a sliding-window counter
(current window + previous window weighted by overlap). While the rate is over `fg_threshold`
every fork is refused with `-EAGAIN` and the caller sleeps `fg_throttle_ms`
(`schedule_timeout_killable`, never while holding the spinlock).
1. **THROTTLE** – the first fork over the threshold.
2. **FREEZE** – still over the threshold one full window after throttling began: `SIGSTOP` to
   the caller's process group. A bursty but legitimate job slows down under throttling and
   never gets here.
3. **REAP** – a delayed work item runs one window after the freeze. A stopped group cannot
   fork, so its rate can't be measured any more; if the group is still alive (nobody resumed
   or killed it) it is `SIGKILL`ed. **SIGKILL is sent before SIGCONT**: SIGKILL wakes stopped
   tasks anyway, while SIGCONT first would let the bomb run and fork again.

The *process group* is targeted because a fork bomb is many processes; killing one leaves
the rest. Since the bomb is its own job, the shell gets its prompt back without help.
After `fg_cooldown_s` with the rate back to normal, the user returns to stage NONE.
Root and kernel threads are exempt. PID 1's process group is never stopped or killed.

**Tiers.** GOLD = nice −7 (weight 4904), SILVER = 0 (1024, default), BRONZE = +5 (335).
The bias is clamped to [−7, +5]: in the worst case, GOLD against BRONZE on one core,
BRONZE still gets 335 / (4904 + 335) ≈ 6.4 % of the CPU, so it never starves.
`sched_fork()` sets `static_prio` + `set_load_weight()` for new tasks of GOLD/BRONZE users
(SILVER users keep normal `nice` inheritance). A tier change walks every running thread
(`for_each_process_thread` under RCU) and calls `set_user_nice()`, so jobs already in flight
are re-tiered immediately. Only one GOLD: setting a second demotes the first to SILVER.

**Safety over privilege.** `fork_guard_check()` runs at the very top of `kernel_clone()` and
never reads the tier, so a GOLD user's fork bomb is contained identically.

**Locking.** One spinlock (`spin_lock_irqsave`) protects the per-user hash table. Nothing
sleeps or calls into the scheduler/signal code while holding it; process-group references
are held with `get_pid()`/`put_pid()`.

## Tested

Built from the patch on vanilla Linux 6.8 (`W=1`, no warnings) and booted in QEMU with a
busybox + bash initramfs (`qemu_test_init.sh` is its `/init`; full log in
`qemu_test_output.log`). Results, one CPU:

| Test | Result |
|---|---|
| 1 baseline | alice 49% / bob 50% |
| 2 alice GOLD (job already running) | 83% / 16% |
| 3 bob BRONZE | 93% / 6% (bob still progressing) |
| 4 reset | 50% / 49% |
| 5 `:(){ :\|:& };:` (threshold 50) | THROTTLE → FREEZE → REAP in ~2 s, 0 processes left |
| 6 same as GOLD | identical ladder |
| 7 `fg_threshold=10` | contained with fewer processes spawned (9 vs 44) |
| 8 `/proc/fork_guard` | counters 3/3/3, stage back to NONE after cooldown |

Also checked: a second GOLD demotes the first; non-root `tier set` and invalid tiers are
rejected; a child forked by a GOLD user starts at nice −7; 40 forks under the threshold all succeed.
