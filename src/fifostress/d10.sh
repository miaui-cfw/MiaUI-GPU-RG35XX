#!/bin/sh
# d10.sh [SECS] : (device, tests only) D10: does the GPU driver's 3-buffer rotation (scan-out at yoffset 0 / 480 /
#   960) matter? panflip tri (no GL) under wcstress (WC stores ~1.4 GB/s): with the OMAPLFB-style fb_set_var flip
#   4 ms into the scan, and with a plain pan right after vsync (fb0 set to 3 screens first); then idle control.
#   MinUI parked, 1.296 GHz, all restored after. /dev/mem reads only. Writes nothing to the SD card.
SECS=${1:-20}; D=$(dirname "$0")
echo -17 > /proc/$$/oom_adj
OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 1
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
run() {  # run LOAD PANFLIP-ARGS...
  L=$1; shift
  printf "load %-4s %-26s: " $L "$*"
  [ $L = wc ] && { $D/wcstress $((SECS + 1)) --mode set >/dev/null & W=$!; }
  $D/panflip $SECS tri "$@" | grep RESULT
  [ $L = wc ] && wait $W
}
run wc --at 4000 --setvar
fbset -g 640 480 640 1440 32
run wc --at 0
run none --at 4000 --setvar
fbset -g 640 480 640 960 32
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
