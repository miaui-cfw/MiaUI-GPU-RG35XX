#!/bin/sh
# d17.sh [SECS] : (device, tests only) D17: the DDR picture of D16 (desampler: DE + ALL / IDLE / CPU at 50 us) in the
#   two configurations that do NOT underflow, with the same CPU stream (wcstress, WC stores ~1.4 GB/s):
#     ns   GPU renders without swapping (fifostress --fill 0 --no-swap), panflip same on screen 4 (D15 run 5: 0)
#     ng   no GPU at all, panflip same on screen 4 (D7: 0)
#   For each, PC1 = ALL (10, rw), IDLE (11), CPU (0, rw). With no underflow, desampler keeps reference frames.
#   Dumps: /tmp/ds17/{ns,ng}-{all,idle,cpu}.txt (RAM), pulled and decoded on the PC.
#   MinUI parked, fb0 5 screens, 1.296 GHz, restored after. /dev/mem reads only. Writes nothing to the SD card.
SECS=${1:-20}; D=$(cd "$(dirname "$0")" && pwd)
echo -17 > /proc/$$/oom_adj
OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 2
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
mkdir -p /tmp/ds17
grep -q "^desampler " /proc/modules && rmmod desampler
for cfg in ns ng; do
  for m in "all 10 2" "idle 11 0" "cpu 0 2"; do
    set -- $m
    fbset -g 640 480 640 2400 32
    insmod $D/desampler.ko m_id=5,$2 m_mode=0,$3 ref_every=30 || { echo "desampler.ko did not load"; exit 1; }
    echo clear > /proc/desampler
    $D/wcstress $((SECS + 2)) --mode set >/dev/null & W=$!
    F=
    [ $cfg = ns ] && { $D/fifostress -t $((SECS + 1)) -r $((SECS + 1)) --fill 0 --no-swap >/dev/null 2>&1 & F=$!; }
    sleep 1; printf "%-3s %-5s: " $cfg $1; $D/panflip $((SECS - 2)) same --base 4 | grep RESULT
    [ -n "$F" ] && wait $F; wait $W
    cat /proc/desampler > /tmp/ds17/$cfg-$1.txt
    rmmod desampler
  done
done
fbset -g 640 480 640 960 32
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
ls -l /tmp/ds17
