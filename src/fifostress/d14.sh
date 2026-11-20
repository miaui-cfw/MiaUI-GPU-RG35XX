#!/bin/sh
# d14.sh [SECS] : (device, tests only) D14: the GPU renders and swaps exactly as normal (fifostress --fill 0, the
#   minimal clear + swap cycle) with wcstress beside it (WC stores ~1.4 GB/s), but noflip.ko turns the GPU driver's
#   flip (OMAPLFBFlip -> fb_set_var) into a no-op, so the GPU never reprograms the display:
#     1  static display, panflip only watching the sticky DE_OUTPUT_STAT
#     2  panflip flipping alternately right after vsync (FBIOPAN_DISPLAY)
#     3  panflip flipping the way the GPU driver does (fb_set_var, 4 ms into the scan)
#   then noflip.ko removed and the reproducer once as usual (control: hundreds expected).
#   In 1-3 fifostress's own underflow count is meaningless (it counts the driver's printk, which only flips make):
#   read panflip's line, and fifostress's fps (must stay ~60: the GPU side ran normally).
#   MinUI parked, 1.296 GHz, restored after. /dev/mem reads only. Writes nothing to the SD card.
SECS=${1:-20}; D=$(cd "$(dirname "$0")" && pwd)
echo -17 > /proc/$$/oom_adj
OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 2
fbset -g 640 480 640 960 32
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
insmod $D/noflip.ko || { echo "noflip.ko did not load (see dmesg)"; exit 1; }
dmesg | grep noflip | tail -1
run() {  # run NAME PANFLIP-ARGS...
  N=$1; shift
  echo "$N"
  $D/wcstress $((SECS + 2)) --mode set >/dev/null & W=$!
  $D/fifostress -t $((SECS + 1)) -r $((SECS + 1)) --fill 0 2>&1 | grep RESULT | sed 's/^/  fifostress /' & F=$!
  sleep 1; $D/panflip $((SECS - 2)) "$@" | grep RESULT | sed 's/^/  /'
  wait $F; wait $W
}
run "1: GPU swaps, its flips skipped, static display" none
run "2: GPU swaps, its flips skipped, panflip alt at vsync" alt
run "3: GPU swaps, its flips skipped, panflip alt setvar at 4 ms" alt --at 4000 --setvar
echo "GPU flips skipped: $(cat /sys/module/noflip/parameters/skipped)"
rmmod noflip; dmesg | grep noflip | tail -1
fbset -g 640 480 640 960 32
echo "4: control, GPU flips back"
$D/wcstress $((SECS + 1)) --mode set >/dev/null & W=$!
$D/fifostress -t $SECS -r $SECS --fill 0 2>&1 | grep RESULT | sed 's/^/  /'
wait $W
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
