#!/bin/sh
# d7.sh [SECS] : (device, tests only) D7 "same-address flip" test. For each mode (none, same, alt) runs panflip
#   (hand-driven fb0 pans, one per vsync, no GL) for SECS with wcstress beside it (WC stores ~1.4 GB/s into fb0
#   screens 6-9), then the same three modes without wcstress as controls. MinUI parked, 1.296 GHz, restored after.
#   Reads DE_OUTPUT_STAT through /dev/mem (read only). Writes nothing to the SD card.
SECS=${1:-20}; D=$(dirname "$0")
echo -17 > /proc/$$/oom_adj
OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 1
fbset -g 640 480 640 960 32   # 2 screens for mode alt (the GPU driver leaves yres_virtual at its last flip)
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
for load in wc none; do
  for mode in none same alt; do
    printf "load %-4s mode %-4s: " $load $mode
    [ $load = wc ] && { $D/wcstress $((SECS + 1)) --mode set >/dev/null & W=$!; }
    $D/panflip $SECS $mode | grep RESULT
    [ $load = wc ] && wait $W
  done
done
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
