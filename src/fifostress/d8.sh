#!/bin/sh
# d8.sh [SECS] : (device, tests only) D8: which part of the GPU driver's flip makes the DE FIFO fragile. Runs panflip
#   in mode alt with wcstress beside it (WC stores ~1.4 GB/s), crossing the flip time (right after vsync, 4 ms and
#   8 ms into the scan) with the flip call (FBIOPAN_DISPLAY, as d7; or fb_set_var exactly as OMAPLFBFlip does), then
#   the worst case without wcstress as the control. MinUI parked, fb0 set to 2 screens for the pans, 1.296 GHz,
#   all restored after. Reads DE_OUTPUT_STAT through /dev/mem (read only). Writes nothing to the SD card.
SECS=${1:-20}; D=$(dirname "$0")
echo -17 > /proc/$$/oom_adj
OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 1
fbset -g 640 480 640 960 32
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
run() {  # run LOAD PANFLIP-ARGS...
  L=$1; shift
  printf "load %-4s %-22s: " $L "$*"
  [ $L = wc ] && { $D/wcstress $((SECS + 1)) --mode set >/dev/null & W=$!; }
  $D/panflip $SECS alt "$@" | grep RESULT
  [ $L = wc ] && wait $W
}
for at in 0 4000 8000; do
  run wc --at $at
  run wc --at $at --setvar
done
run none --at 4000 --setvar
fbset -g 640 480 640 960 32
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
