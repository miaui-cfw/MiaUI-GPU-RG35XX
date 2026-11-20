#!/bin/sh
# d18.sh [SECS] : (device, tests only) D18: is the PowerVR driver's per-frame power cycle (SGX state save, power
#   off 2 ms after each frame, wake + state restore at the next work: ~86 a second) what starves the display?
#   apmlat.ko counts GPU power-ups and can raise the SGX active-power latency (2 ms -> 500 ms), so the GPU stays up
#   between frames. Reproducer: fifostress --fill 0 (GPU flips) + wcstress (WC stores ~1.4 GB/s).
#     normal x2 (latency 2 ms, power-ups counted), latency 500 ms x2, back to 2 ms x1.
#   MinUI parked, 1.296 GHz, restored after. Writes nothing to the SD card.
SECS=${1:-20}; D=$(cd "$(dirname "$0")" && pwd); P=/sys/module/apmlat/parameters
echo -17 > /proc/$$/oom_adj
OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 2
fbset -g 640 480 640 960 32
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
insmod $D/apmlat.ko || { echo "apmlat.ko did not load (see dmesg)"; exit 1; }
run() {
  U0=$(cat $P/powerups)
  printf "latency %-4s: " "$1"
  $D/wcstress $((SECS + 1)) --mode set >/dev/null & W=$!
  $D/fifostress -t $SECS -r $SECS --fill 0 2>&1 | grep RESULT | tr -d '\n'
  wait $W
  echo "  GPU power-ups $(( $(cat $P/powerups) - U0 ))"
  sleep 2   # let the GPU power down, so the next power-up takes the new latency
}
run 2; run 2
echo 500 > $P/latency_ms; sleep 1
run 500; run 500
echo 0 > $P/latency_ms; sleep 2
run 2
echo "rewritten: $(cat $P/rewritten)"
rmmod apmlat; dmesg | grep apmlat | tail -1
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
