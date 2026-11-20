#!/bin/sh
# trace-uf.sh SECS [min|probes|sched|full] -- CMD... : (device, tests only; needs the source-built kernel with KPROBE_EVENT)
#   Runs CMD at MinUI's game clock (1.2 GHz) with MinUI parked and records, for decoding on the PC (trace_uf.py):
#   - every DE flip "go" (de_path_fcr_set) and every flip that reports a fifo underflow (kprobe on the printk branch,
#     with DE_OUTPUT_STAT), owlfb pan/vsync/dc events;
#   - every CPU cache maintenance entry the kernel and the PVR module use (L1 v7_*, L2 l2x0_*, PVR OS*CPUCache*), with
#     the address range, and returns of the whole-cache ones (their duration);
#   - IRQ entry/exit, and (sched/full) scheduler switches and wake-ups;
#   - GPU power/clock gating (PVR + owl powergate), CPU hotplug;
#   - DDR bandwidth: the kernel's DMM statistics (debugfs dmm, DMM_MASTERS="id0 mode0 id1 mode1", default DE read +
#     GPU3D read/write) read at every IRQ, flip and underflow (cumulative 16-byte units, reset every second).
#   min: only the flip/underflow/IRQ counter probes and owlfb events (least perturbation).
#   Raw ring-buffer pages + event formats go to /tmp/cap (RAM). Writes nothing to the SD card. MinUI restored after.
SECS=$1; MODE=$2; shift 3
T=/sys/kernel/debug/tracing; O=/tmp/cap
echo -17 > /proc/$$/oom_adj
rm -rf $O; mkdir -p $O
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
echo 0 > $T/tracing_on; echo nop > $T/current_tracer; echo > $T/trace; echo 0 > $T/events/enable
echo > $T/kprobe_events
echo 6144 > $T/buffer_size_kb; echo 16384 > $T/per_cpu/cpu0/buffer_size_kb; echo local > $T/trace_clock
# DMM performance counters (owl-ddr-debug.c): struct dmm at 0xc0bb1b48 in this build, DMM_PC0/PC1 pointers +148/+152
D=/sys/kernel/debug/dmm; set -- ${DMM_MASTERS:-5 0 6 2} "$@"
echo $1 > $D/master0/id; echo $2 > $D/master0/mode; echo $3 > $D/master1/id; echo $4 > $D/master1/mode; shift 4
echo 1000 > $D/sampling_rate; echo 1 > $D/enable; sleep 2
PC='pc0=+0(@0xc0bb1bdc):u32 pc1=+0(@0xc0bb1be0):u32'

# kprobes: name|definition. A probe the kernel refuses is reported and skipped.
kp() {
  echo "$1" >> $T/kprobe_events 2>/dev/null || echo "kprobe refused: $1"
}
kp "p:uf de_path_fcr_set+0x250 stat=%r5:u32 $PC"
kp "p:fcr de_path_fcr_set path=%r0:u32 $PC"
kp "p:pan owlfb_pan_display $PC"
kp "p:hirq handle_IRQ irq=%r0:u32 $PC"
kp 'p:dmmrst hr_timer_func'
if [ "$MODE" != min ]; then
kp 'p:sgxon EnableSGXClocks'
kp 'p:sgxoff DisableSGXClocks'
kp 'p:gclkon owl_gpu_clk_enable'
kp 'p:gclkoff owl_gpu_clk_disable'
kp 'p:pgon owl_powergate_power_on id=%r0:u32'
kp 'p:pgoff owl_powergate_power_off id=%r0:u32'
kp 'p:gpgon gpu_dvfs_notify_power_gate_on'
kp 'p:gpgoff gpu_dvfs_notify_power_gate_off'
kp 'p:cpuup _cpu_up cpu=%r0:u32'
kp 'p:cpudown _cpu_down cpu=%r0:u32'
kp 'p:l2fr l2x0_flush_range s=%r0:u32 e=%r1:u32'
kp 'p:l2cr l2x0_clean_range s=%r0:u32 e=%r1:u32'
kp 'p:l2ir l2x0_inv_range s=%r0:u32 e=%r1:u32'
kp 'p:l2fa l2x0_flush_all'
kp 'r:l2fa_ret l2x0_flush_all'
kp 'p:l1fa v7_flush_kern_cache_all'
kp 'r:l1fa_ret v7_flush_kern_cache_all'
kp 'p:l1fr v7_dma_flush_range s=%r0:u32 e=%r1:u32'
kp 'p:l1cr v7_dma_clean_range s=%r0:u32 e=%r1:u32'
kp 'p:l1ir v7_dma_inv_range s=%r0:u32 e=%r1:u32'
kp 'p:l1da v7_flush_kern_dcache_area a=%r0:u32 n=%r1:u32'
kp 'p:pvfl OSFlushCPUCacheKM'
kp 'p:pvcl OSCleanCPUCacheKM'
kp 'p:pvflr OSFlushCPUCacheRangeKM'
kp 'p:pvclr OSCleanCPUCacheRangeKM'
kp 'p:pvinr OSInvalidateCPUCacheRangeKM'
fi
echo 1 > $T/events/kprobes/enable
echo 1 > $T/events/owlfb-trace-events/enable
[ "$MODE" != min ] && echo 1 > $T/events/irq/irq_handler_entry/enable
[ "$MODE" != min ] && echo 1 > $T/events/irq/irq_handler_exit/enable
if [ "$MODE" != probes ] && [ "$MODE" != min ]; then
  echo 1 > $T/events/sched/sched_switch/enable
  [ "$MODE" = full ] && echo 1 > $T/events/sched/sched_wakeup/enable
fi

OLD=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
/etc/init.d/S31minui stop >/dev/null 2>&1; sleep 1
/opt/minui/bin/overclock.elf 1296000 >/dev/null 2>&1
cut -d' ' -f1 /proc/uptime > $O/t0
echo 1 > $T/tracing_on
"$@" > $O/app.log 2>&1 &
P=$!
sleep $SECS
echo 0 > $T/tracing_on
echo 0 > $D/enable
# read the buffers while the app still runs (and every CPU is busy/online), then stop it
for c in 0 1 2 3; do cat $T/per_cpu/cpu$c/trace_pipe_raw > $O/raw$c & K=$!; sleep 4; kill $K; done
kill $P; sleep 2; kill -9 $P 2>/dev/null
for c in 0 1 2 3; do grep -E "overrun|entries" $T/per_cpu/cpu$c/stats | tr '\n' ' ' | sed "s/^/cpu$c: /"; echo; done > $O/stats
cp $T/events/header_page $T/events/header_event $O/
for d in $T/events/kprobes/*/ $T/events/irq/irq_handler_entry/ $T/events/irq/irq_handler_exit/ \
         $T/events/sched/sched_switch/ $T/events/sched/sched_wakeup/ $T/events/owlfb-trace-events/*/; do
  [ -f $d/format ] && cp $d/format $O/fmt_$(basename $d)
done
echo 0 > $T/events/enable; echo > $T/kprobe_events; echo > $T/trace; echo 1408 > $T/buffer_size_kb
umount /sys/kernel/debug
dmesg | grep underflow > $O/dmesg_uf
cat /proc/interrupts > $O/interrupts
ls -l $O | awk '{print $5, $9}' | tr '\n' ' '; echo; cat $O/stats
/opt/minui/bin/overclock.elf "$OLD" >/dev/null 2>&1
setsid /usr/bin/minui </dev/null >/dev/null 2>&1 &
sleep 3
