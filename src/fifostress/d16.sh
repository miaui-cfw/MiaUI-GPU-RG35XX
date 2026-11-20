#!/bin/sh
# d16.sh [SECS] : (device, tests only) D16: what else is on the DDR when the DE starves? The 20 s reproducer
#   (fifostress --fill 0 GPU flips + wcstress WC stores) three times under desampler.ko (50 us DMM sampling around
#   every vblank; only the first counter pair works), each time DE read + one other DMM master:
#     all   master 10 (ALL, read+write)   total DDR traffic: compare with DE + CPU + GPU
#     idle  master 11 (IDLE)              DDR idle cycles: ~0 at the underflow = DDR saturated, high = arbitration
#     cpu   master 0 (CPU, read+write)    the CPU stream in the same conditions
#   Dumps go to /tmp/ds16/{all,idle,cpu}.txt (RAM); the PC pulls and decodes them (desampler.py).
#   MinUI parked, 1.296 GHz, restored after. Writes nothing to the SD card.
SECS=${1:-20}; D=$(cd "$(dirname "$0")" && pwd)
echo -17 > /proc/$$/oom_adj
OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 2
fbset -g 640 480 640 960 32
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
mkdir -p /tmp/ds16
grep -q "^desampler " /proc/modules && rmmod desampler
for m in "all 10 2" "idle 11 0" "cpu 0 2"; do
  set -- $m
  insmod $D/desampler.ko m_id=5,$2 m_mode=0,$3 || { echo "desampler.ko did not load"; break; }
  echo clear > /proc/desampler
  $D/wcstress $((SECS + 1)) --mode set >/dev/null & W=$!
  printf "%-5s: " $1; $D/fifostress -t $SECS -r $SECS --fill 0 2>&1 | grep RESULT
  wait $W
  cat /proc/desampler > /tmp/ds16/$1.txt
  rmmod desampler
  echo "       $(head -1 /tmp/ds16/$1.txt | cut -c1-90)"
done
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
