#!/bin/sh
# dmm-ids.sh : (device, tests only) which DMM master id carries which traffic: runs the AITD2 intro (MinUI parked,
#   1.2 GHz) and, after 25 s, counts each master id 0-9 (read+write) next to ALL for one second each, through the
#   kernel's debugfs dmm statistics. Writes nothing to the SD card. MinUI restored after.
echo -17 > /proc/$$/oom_adj
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
D=/sys/kernel/debug/dmm
OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 1
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
/tmp/fs/aitd-old-plain.sh >/dev/null 2>&1 &
P=$!
sleep 25
echo 500 > $D/sampling_rate
for mode in ${MODES:-2}; do
for id in 0 1 2 3 4 5 6 7 8 9; do
  echo $id > $D/master0/id; echo $mode > $D/master0/mode; echo 10 > $D/master1/id; echo 2 > $D/master1/mode
  echo 1 > $D/enable; sleep 1.2; echo 0 > $D/enable
  echo "id $id mode $mode: $(tail -n +2 $D/result | tail -1)"
done
done
kill $P; sleep 2; kill -9 $P 2>/dev/null
umount /sys/kernel/debug
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
