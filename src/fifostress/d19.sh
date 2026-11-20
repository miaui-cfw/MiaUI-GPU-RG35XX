#!/bin/sh
# d19.sh [SECS] : (device, tests only) D19: what does the PowerVR driver do just before the GPU's mid-frame DDR
#   bursts that precede each display underflow? Runs the reproducer (fifostress --fill 0, GPU flips, + wcstress WC
#   stores) for SECS (default 10) with:
#     - desampler.ko: DE read + GPU ("SI", master 7, rw) DDR rates every 50 us around every vblank;
#     - kprobes on the PVR driver (entry only): render kicks, transfer (blit) submits, swaps, flip command, swap
#       work, command completion, queue processing, SGX ISR/MISR, active-power tests, power state changes.
#   Both use local_clock(), so events and samples line up. Raw ring buffers + formats -> /tmp/cap19, desampler dump
#   -> /tmp/cap19/ds.txt (RAM); the PC pulls and decodes them. MinUI parked, 1.296 GHz, restored after.
#   Writes nothing to the SD card.
SECS=${1:-10}; D=$(cd "$(dirname "$0")" && pwd); T=/sys/kernel/debug/tracing; O=/tmp/cap19
echo -17 > /proc/$$/oom_adj
rm -rf $O; mkdir -p $O
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 2
fbset -g 640 480 640 960 32
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
echo 0 > $T/tracing_on; echo nop > $T/current_tracer; echo > $T/trace; echo 0 > $T/events/enable
echo > $T/kprobe_events; echo local > $T/trace_clock; echo 4096 > $T/buffer_size_kb
for f in SGXDoKickKM SGXSubmitTransferKM PVRSRVSwapToDCBufferKM PVRSRVSwapToDCBuffer2KM ProcessFlip \
         OMAPLFBSwapHandler PVRSRVCommandCompleteKM PVRSRVProcessQueues SGXScheduleProcessQueuesKM \
         SGX_ISRHandler SGX_MISRHandler SGXTestActivePowerEvent SGXPrePowerState SGXPostPowerState \
         PVRSRVSetDevicePowerStateKM; do
  echo "p:$f $f" >> $T/kprobe_events 2>/dev/null || echo "kprobe refused: $f"
done
echo 1 > $T/events/kprobes/enable
grep -q "^desampler " /proc/modules && rmmod desampler
insmod $D/desampler.ko m_id=5,7 m_mode=0,2 ref_every=10 || { echo "desampler.ko did not load"; exit 1; }
echo clear > /proc/desampler
echo 1 > $T/tracing_on
$D/wcstress $((SECS + 1)) --mode set >/dev/null & W=$!
$D/fifostress -t $SECS -r $SECS --fill 0 2>&1 | grep RESULT
wait $W
echo 0 > $T/tracing_on
cat /proc/desampler > $O/ds.txt; rmmod desampler
for c in 0 1 2 3; do cat $T/per_cpu/cpu$c/trace_pipe_raw > $O/raw$c & K=$!; sleep 3; kill $K; done
for c in 0 1 2 3; do grep -E "overrun|entries" $T/per_cpu/cpu$c/stats | tr '\n' ' ' | sed "s/^/cpu$c: /"; echo; done > $O/stats
cp $T/events/header_page $T/events/header_event $O/
for d in $T/events/kprobes/*/; do [ -f $d/format ] && cp $d/format $O/fmt_$(basename $d); done
echo 0 > $T/events/enable; echo > $T/kprobe_events; echo > $T/trace; echo 1408 > $T/buffer_size_kb
umount /sys/kernel/debug
cat $O/stats; ls -l $O | awk '{s+=$5} END {print "capture", s/1024, "KB"}'
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
