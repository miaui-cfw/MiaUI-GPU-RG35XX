#!/bin/sh
# d20.sh [SECS] : (device, tests only) D20: do the SGX544 memory-interface burst bits change the starvation?
#   bifctl.ko applies EUR_CR_BIF_MMU_CTRL bit 3 (ENABLE_WRITE_BURST_COLLATE) / bit 5 (DISABLE_BURST_EXP) after every
#   SGXReset (each GPU power-up: SGX powered + clocked, microkernel not started yet). Reproducer: fifostress
#   --fill 0 (GPU flips) + wcstress (WC stores ~1.4 GB/s); each run prints the register value seen.
#     default x1, collate (0x08) x2, no burst expansion (0x20) x2, both (0x28) x2, default x1.
#   MinUI parked, 1.296 GHz, restored after. Writes nothing to the SD card.
SECS=${1:-20}; D=$(cd "$(dirname "$0")" && pwd); P=/sys/module/bifctl/parameters
echo -17 > /proc/$$/oom_adj
OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 2
fbset -g 640 480 640 960 32
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
insmod $D/bifctl.ko || { echo "bifctl.ko did not load (see dmesg)"; exit 1; }
run() {  # run SET_MASK
  echo $1 > $P/set_mask; sleep 2     # GPU idle -> powers off; the next power-up applies the mask
  R0=$(cat $P/resets)
  printf "set 0x%02x: " $1
  $D/wcstress $((SECS + 1)) --mode set >/dev/null & W=$!
  $D/fifostress -t $SECS -r $SECS --fill 0 2>&1 | grep RESULT | tr -d '\n'
  wait $W
  printf "  resets %d, BIF_MMU_CTRL 0x%x -> 0x%x\n" $(( $(cat $P/resets) - R0 )) $(cat $P/before) $(cat $P/after)
}
run 0; run 8; run 8; run 32; run 32; run 40; run 40; run 0
rmmod bifctl; dmesg | grep bifctl | tail -1
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
