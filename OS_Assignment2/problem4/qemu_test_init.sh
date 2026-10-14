#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev
echo 1 > /proc/sys/kernel/printk 2>/dev/null

cpu_of() { # total utime+stime ticks of all processes of uid $1
  t=0
  for d in /proc/[0-9]*; do
    u=$(awk '/^Uid:/{print $2}' $d/status 2>/dev/null)
    [ "$u" = "$1" ] || continue
    s=$(cut -d' ' -f14,15 $d/stat 2>/dev/null) || continue
    set -- $1 $s; t=$((t + $2 + $3))
  done
  echo $t
}
share() { # measure CPU split between alice and bob over $1 seconds
  a0=$(cpu_of 1000); b0=$(cpu_of 1001); sleep $1
  a1=$(cpu_of 1000); b1=$(cpu_of 1001)
  da=$((a1-a0)); db=$((b1-b0)); tot=$((da+db)); [ $tot -gt 0 ] || tot=1
  echo "  alice=$((100*da/tot))% bob=$((100*db/tot))%  (ticks alice=$da bob=$db)"
}
nprocs() { n=0; for d in /proc/[0-9]*; do u=$(awk '/^Uid:/{print $2}' $d/status 2>/dev/null); [ "$u" = "$1" ] && n=$((n+1)); done; echo $n; }

echo "=== BOOT"; uname -r; cat /proc/fork_guard; ls /proc/sys/kernel/ | grep fg_

echo "=== T1 baseline (both SILVER)"
su -s /bin/sh alice -c 'while :; do :; done' &
su -s /bin/sh bob -c 'while :; do :; done' &
sleep 2; share 6
echo "=== T2 promote alice GOLD (running job)"
tier set alice gold; share 6
echo "  nice of alice procs:"; ps -o user,nice,comm | grep alice
echo "=== T3 demote bob BRONZE"
tier set bob bronze; share 6
echo "=== T3b only one GOLD: set bob gold"; tier set bob gold; tier show | grep -E "^10"
echo "=== T4 reset"; tier reset; share 6
echo "=== non-root tier set must fail"; su -s /bin/sh alice -c 'tier set alice gold'; echo "  exit=$?"
echo "=== bad tier must fail"; tier set alice platinum; echo "  exit=$?"
killall sh 2>/dev/null; sleep 1
sleep 1

echo 50 > /proc/sys/kernel/fg_threshold
echo 2 > /proc/sys/kernel/fg_cooldown_s
bomb_test() {
  dmesg -c >/dev/null
  setsid su -s /bin/bash alice -c ':(){ :|:& };:' &
  for i in $(seq 1 40); do
    sleep 1; n=$(nprocs 1000); echo "  t=${i}s alice procs=$n"
    [ $i -gt 3 ] && [ "$n" = 0 ] && break
  done
  dmesg | grep fork_guard
  cat /proc/fork_guard | grep -E "^1000|UID"
}
echo "=== T5 containment, untiered (threshold 50)"; bomb_test
echo "=== T6 containment, alice GOLD"; tier set alice gold; bomb_test
echo "=== child inherits GOLD nice:"; su -s /bin/sh alice -c 'cut -d" " -f19 /proc/self/stat; true'
tier reset
echo "=== T7 retune threshold 10"; echo 10 > /proc/sys/kernel/fg_threshold; bomb_test
echo "=== cooldown: wait 4s"; sleep 4; echo "  remaining alice procs: $(nprocs 1000)"; su -s /bin/sh alice -c 'true'; tier show
echo "=== legit burst under threshold (40 forks, threshold 50) must succeed"
echo 50 > /proc/sys/kernel/fg_threshold; sleep 2
su -s /bin/sh alice -c 'ok=0; i=0; while [ $i -lt 40 ]; do /bin/true && ok=$((ok+1)); i=$((i+1)); done; echo "  ok=$ok/40"'
echo "=== DONE"
poweroff -f
