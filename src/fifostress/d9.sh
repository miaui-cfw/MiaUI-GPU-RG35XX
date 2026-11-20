#!/bin/sh
# d9.sh [SECS] : (device, tests only) D9, all with wcstress beside (WC stores ~1.4 GB/s):
#   A  positive control: the 20 s reproducer (fifostress GL flips) with panflip in mode none only WATCHING the
#      sticky DE_OUTPUT_STAT at each vsync; it must report SEEN, or panflip's counter (D7/D8) cannot see underflows;
#   B  GPU rendering without GPU flips: fifostress --fill 1 --no-swap (renders + glFinish, its driver never flips)
#      with panflip flipping alternately right after vsync;
#   C  same as B with the flips as the GPU driver makes them (fb_set_var, 4 ms into the scan).
#   MinUI parked, fb0 2 screens, 1.296 GHz, restored after. /dev/mem reads only. Writes nothing to the SD card.
SECS=${1:-20}; D=$(dirname "$0")
echo -17 > /proc/$$/oom_adj
OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 1
fbset -g 640 480 640 960 32
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
cd $D
echo "A: fifostress flips + wcstress, panflip watching"
./wcstress $((SECS + 2)) --mode set >/dev/null & W=$!
./fifostress -t $SECS -r $SECS --fill 1 2>&1 | grep RESULT & F=$!
sleep 1; ./panflip $((SECS - 2)) none | grep RESULT
wait $F; wait $W
for c in "B:" "C: --at 4000 --setvar"; do
  set -- $c; N=$1; shift
  echo "$N fifostress --no-swap + wcstress + panflip alt $*"
  fbset -g 640 480 640 960 32
  ./wcstress $((SECS + 2)) --mode set >/dev/null & W=$!
  ./fifostress -t $((SECS + 1)) -r $((SECS + 1)) --fill 1 --no-swap 2>&1 | grep RESULT & F=$!
  sleep 1; ./panflip $((SECS - 2)) alt "$@" | grep RESULT
  wait $F; wait $W
done
fbset -g 640 480 640 960 32
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
