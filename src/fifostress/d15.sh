#!/bin/sh
# d15.sh [SECS] : (device, tests only) D15: does it matter WHERE the display scans relative to the buffers the GPU
#   renders into? noflip.ko loaded (the GPU renders + swaps as normal, fifostress --fill 0, into fb0 screens 0-2,
#   but never flips the display); panflip shows a chosen part of fb0; wcstress (WC stores ~1.4 GB/s) writes
#   screens 6-9:
#     1  same, screen 0     the display scans a buffer the GPU renders into every 3rd frame
#     2  same, screen 4     a screen nobody writes
#     3  alt, screens 3/4   page flips between two screens nobody writes
#     4  same, screen 4, no wcstress   (GPU only)
#     5  same, screen 4, wcstress, fifostress --no-swap   (GPU renders, never swaps)
#   MinUI parked, fb0 set to 5 screens, 1.296 GHz, all restored after. /dev/mem reads only. Writes nothing to the
#   SD card.
SECS=${1:-20}; D=$(cd "$(dirname "$0")" && pwd)
echo -17 > /proc/$$/oom_adj
OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 2
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
insmod $D/noflip.ko || { echo "noflip.ko did not load (see dmesg)"; exit 1; }
run() {  # run NAME LOAD SWAP PANFLIP-ARGS...
  N=$1; L=$2; S=$3; shift 3
  echo "$N"
  fbset -g 640 480 640 2400 32
  [ $L = wc ] && { $D/wcstress $((SECS + 2)) --mode set >/dev/null & W=$!; }
  $D/fifostress -t $((SECS + 1)) -r $((SECS + 1)) --fill 0 $S 2>&1 | grep RESULT | sed 's/^/  fifostress /' & F=$!
  sleep 1; $D/panflip $((SECS - 2)) "$@" | grep RESULT | sed 's/^/  /'
  wait $F; [ $L = wc ] && wait $W
}
run "1: same, screen 0 (GPU renders into it)" wc "" same --base 0
run "2: same, screen 4 (nobody writes it)" wc "" same --base 4
run "3: alt, screens 3/4" wc "" alt --base 3
run "4: same, screen 4, no wcstress" none "" same --base 4
run "5: same, screen 4, GPU --no-swap" wc --no-swap same --base 4
echo "GPU flips skipped: $(cat /sys/module/noflip/parameters/skipped)"
rmmod noflip
fbset -g 640 480 640 960 32
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
