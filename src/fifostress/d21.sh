#!/bin/sh
# d21.sh [SECS] : (device, tests only; DDR controller writes, user-approved) D21: does DMM_AXI_PRIORITY (+0x2C),
#   which LeMaker's bootloader sets to 2 then 6 but the console's leaves at 0, change the CPU vs NoC arbitration?
#   Reproducer (fifostress --fill 0 GPU flips + wcstress WC stores) at +0x2C = 0 (x1), 2 (x2), 6 (x2), then
#   dmm_port.sh --restore (+0x2C back to 0) and once more. Each value is loaded like the bootloader does
#   (SCH_CTRL bit 2 pulse, in dmm_port.sh). Never touches +0x24 (interleave fragment: would remap live memory).
#   MinUI parked, 1.296 GHz, restored after. A reboot restores the DMM too. Writes nothing to the SD card.
SECS=${1:-20}; D=$(cd "$(dirname "$0")" && pwd)
echo -17 > /proc/$$/oom_adj
OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 2
fbset -g 640 480 640 960 32
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
run() {
  printf "AXI_PRIORITY %-7s: " "$1"
  $D/wcstress $((SECS + 1)) --mode set >/dev/null & W=$!
  $D/fifostress -t $SECS -r $SECS --fill 0 2>&1 | grep RESULT
  wait $W; sleep 1
}
run 0
sh $D/dmm_port.sh 0x2c lo 0x2 | tr ' ' '\n' | grep -E '^\+0x(00|2c)='; run 2; run 2
sh $D/dmm_port.sh 0x2c lo 0x6 | tr ' ' '\n' | grep -E '^\+0x(00|2c)='; run 6; run 6
sh $D/dmm_port.sh --restore >/dev/null; sh $D/dmm_port.sh --show | tr ' ' '\n' | grep -E '^\+0x2c='; run restored
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
