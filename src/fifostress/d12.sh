#!/bin/sh
# d12.sh [SECS] : (device, tests only) D12: with the GPU driver's own flips (fifostress GL) and wcstress beside it
#   (WC stores ~1.4 GB/s), vary the GPU's work and its timing, to tell "GPU traffic around the flip" from "the
#   swap path itself":
#     fill 0            clear + swap only (least GPU work: the tile write-back of one 640x480 frame)
#     fill 1            the reproducer as usual
#     fill 8            8 blended full-screen quads (much more GPU traffic)
#     fill 1 flip-at 8  render, glFinish, wait vsync, sleep 8 ms, then swap: the flip lands mid-scan with the GPU
#                       idle, and the next render starts after it
#     fill 1 delay 8    sleep 8 ms before drawing: the GPU work moves 8 ms later in the scan
#     no-swap           GPU renders every frame, nothing flips (static display); panflip only watches the sticky
#                       DE_OUTPUT_STAT
#   MinUI parked, 1.296 GHz, restored after. /dev/mem reads only. Writes nothing to the SD card.
SECS=${1:-20}; D=$(dirname "$0")
echo -17 > /proc/$$/oom_adj
OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 1
fbset -g 640 480 640 960 32
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
cd $D
for args in "--fill 0" "--fill 1" "--fill 8" "--fill 1 --flip-at 8" "--fill 1 --delay 8"; do
  printf "%-22s: " "$args"
  ./wcstress $((SECS + 1)) --mode set >/dev/null & W=$!
  ./fifostress -t $SECS -r $SECS $args 2>&1 | grep RESULT
  wait $W
done
printf "%-22s: " "--fill 1 --no-swap"
./wcstress $((SECS + 2)) --mode set >/dev/null & W=$!
./fifostress -t $((SECS + 1)) -r $((SECS + 1)) --fill 1 --no-swap >/dev/null 2>&1 & F=$!
sleep 1; ./panflip $((SECS - 2)) none | grep RESULT
wait $F; wait $W
fbset -g 640 480 640 960 32
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
