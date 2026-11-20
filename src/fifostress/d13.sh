#!/bin/sh
# d13.sh [SECS] : (device, tests only) D13: is the GPU3D power gating (~86 power cycles a second) the trigger?
#   Runs the 20 s reproducer (fifostress --fill 0 = clear + swap, the GPU driver's own flips, with wcstress beside
#   it) twice as is, then twice with gpuhold.ko loaded (one extra reference on the GPU3D power domain: the GPU
#   driver's power-offs no longer remove power; its clock gating and GPU resets stay), then once more without.
#   The module is loaded and unloaded only while no GL client runs. MinUI parked, 1.296 GHz, restored after.
#   Writes nothing to the SD card.
SECS=${1:-20}; D=$(cd "$(dirname "$0")" && pwd)
echo -17 > /proc/$$/oom_adj
OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 2
fbset -g 640 480 640 960 32
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
pg() { grep -i gpu3d /sys/kernel/debug/powergate | tr -s ' '; }
run() {
  printf "%-10s: " "$1"
  $D/wcstress $((SECS + 1)) --mode set >/dev/null & W=$!
  $D/fifostress -t $SECS -r $SECS --fill 0 2>&1 | grep RESULT
  wait $W; sleep 1
}
echo "powergate before: $(pg)"
run normal; run normal
insmod $D/gpuhold.ko && echo "powergate held: $(pg)"
run held; run held
echo "powergate held, after: $(pg)"
rmmod gpuhold && echo "powergate released: $(pg)"
run normal
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
