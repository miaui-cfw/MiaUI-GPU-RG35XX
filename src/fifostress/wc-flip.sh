#!/bin/sh
# wc-flip.sh SECS [wcstress args...|none] : (device, tests only) parks MinUI, runs fifostress (one blended quad per
#   frame, vsync-locked flips at 60 fps) for SECS with wcstress beside it (or alone with "none"), and prints the
#   underflows. Writes nothing to the SD card. MinUI restored after.
SECS=$1; shift
echo -17 > /proc/$$/oom_adj
OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 1
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
N0=$(dmesg | grep -c underflow)
if [ "$1" != none ]; then /tmp/fs/wcstress $((SECS + 1)) "$@" & W=$!; fi
cd /tmp/fs && ./fifostress -t $SECS -r $SECS --fill 1 2>&1 | tail -2
[ -n "$W" ] && wait $W
echo "underflows (dmesg): $(( $(dmesg | grep -c underflow) - N0 ))"
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
