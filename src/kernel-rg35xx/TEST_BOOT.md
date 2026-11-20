# RG35XX source-built kernel: test boot

The kernel is `build.sh` output: rg35xx-cfw/s500_kernel @81cc77b592, plus `patches/0001-0008`, plus
`config/rg35xx-test.config`, built with Linaro GCC 4.9.4. It identifies itself as
`Linux version 3.10.37 (koriki@rg35xx-test)`.

## What it is

- **Compatibility.** It loads the rootfs modules unchanged: `pvrsrvkm`, `drm`, `gpio_keys_polled` (the gamepad) and
  `atc260x_cap_gauge`.
  - The vermagic is the same (`3.10.37 SMP preempt mod_unload ARMv7 p2v8`), and `struct module` is the same size.
  - All of the modules' symbols resolve, including `allen_adjust_volume`.
- **Config (test #2, kept in test #3).** Rebuilt from the production `rg35xx_atm7039_defconfig`, the one the console was built from
  (test #1 used the debug defconfig, which is why it had no Android USB gadget, so no ADB/USB network, and printed
  boot text).
  - Added, as on the console: the Android gadget (adb, rndis, mass_storage, acm), joydev, gameport/analog joysticks,
    hibernation, earlysuspend, squashfs-xz, PWM LEDs, SPI GPIO.
  - Removed, as on the console: framebuffer console (the boot text), LED triggers/GPIO LEDs, USB tablets except
    wacom, touchscreen, hostap/lib80211, autofs, mousedev, VCE/VDE.
  - Removed for this gaming build: V4L2, media and camera support (no camera on the RG35XX).
  - Test #3: `USB_PLATFORM_LINUX` off, as on the console. With it on, the USB monitor ignores the
    `usb_con_msg` writes `usbnet.sh` uses to switch the port to device mode, so test #2 had no ADB or USB
    network. Patch 0008 adds the console's soft-VBUS and charger-mode handling to the monitor.
  - `tools/configdiff.py` checks it: the initcalls equal the console's, except the tracing additions and three
    non-public vendor drivers (Bluetooth PCM, JTAG switch, upgrade-flag proc).
- **Tracing.** The console's kernel already has `EVENT_TRACING`. This one adds `KPROBES` + `KPROBE_EVENT`,
  `SCHED_TRACER`, `PREEMPT_TRACER` and `FTRACE_SYSCALLS`. None of these change the module ABI.
- **Not added on purpose:** the function/function-graph tracer. It changes `struct module`, so all four rootfs
  modules would need rebuilding first.
- **Unchanged from the console:** HZ=100 and the preemption model. Gaming tuning comes after a baseline trace.

## Known differences from the console's kernel

- **Sound card set up from a work item**, as on the console (test #1 set it up at module init; it had no audio).
- **Non-public vendor code not in the source:** DWC3 USB controller additions (an event-overflow handler that
  replaces the public 500 ms busy-wait, a shutdown hook, a TRB count helper, the USB2 PHY swing from the factory
  calibration value), the "stub" gadget function, the chipid driver, `/proc/hardkey`.
- **No Bluetooth PCM card device.** `/proc/asound/pcm` lists 2 devices instead of 3; there is no `hw:0,2`.
- **Audio DAI/PCM/DMA path.** It is the public one, not the vendor's S900-style rework. Check playback, especially
  mono sources (the console's ALSA plays mono at 2x).
- **HDMI and the backlight driver.** The recovered fixes are ported, but HDMI is not used and was not polished.

## Install (needs the user's OK; touches p1)

On the console:

```sh
mkdir -p /tmp/p1 && mount /dev/mmcblk0p1 /tmp/p1
md5sum /tmp/p1/uImage.koriki   # must be 38e5dc1a1e13601755e75a6fefbe3990 (the working kernel, kept by test #1)
# only if uImage.koriki does not exist yet: cp /tmp/p1/uImage /tmp/p1/uImage.koriki
```

Copy the new image from the PC (`build/uImage-rg35xx-test3`, md5 15c9dfbcc00f1cab9eb7d4bba59459e5), then reboot:

```sh
cp <uImage-rg35xx-test3> /tmp/p1/uImage && sync && umount /tmp/p1 && reboot
```

Without ADB (test #1 or #2 installed), do this from the PC with the SD card: copy `uImage-rg35xx-test3` over `uImage` on
p1 and leave `uImage.koriki` alone.

The pre-loader reads `p1:/uImage` directly (U-Boot's `bootcmd` is `ramboot`, and it does not read `uEnv.txt`). So
there is no side-by-side boot, and the swap is the test.

## Rollback

- **If it boots:**

  ```sh
  mount /dev/mmcblk0p1 /tmp/p1 && cp /tmp/p1/uImage.koriki /tmp/p1/uImage && sync && reboot
  ```

- **If it does not boot:** take the SD card out and, on the PC, copy `uImage.koriki` over `uImage` on the FAT
  partition (p1).

## First-boot checks

1. **Version.** `cat /proc/version` shows `koriki@rg35xx-test`. `lsmod` lists the 4 modules.
2. **Controls.** Gamepad, power key, and the volume keys (`gpio_keys_polled` calls `allen_adjust_volume`).
3. **Display.** MinUI and a game. Watch for the screen shift.
4. **Audio.**
   - Speaker playback.
   - Plug headphones: `cat /sys/class/switch/h2w/state` goes 0 -> 1 and the speaker goes quiet.
   - `/proc/asound/pcm`.
5. **Power.** Rumble (`battery/moto`), the charge and low-battery LEDs, the battery gauge.
6. **USB.** ADB and the USB network from the Quick Menu; `cat /tmp/usbnet.log` shows each step.
7. **Logs first** (to the PC, nothing on the SD): `dmesg > /tmp/dmesg.txt`, `cat /proc/asound/cards
   /proc/asound/pcm`, `ls /dev/snd`, `ls /sys/class/android_usb/android0`, then `adb pull /tmp/dmesg.txt`.
8. **Kernel log.** `dmesg | grep -iE "error|fail|oops|bug"` (some vendor noise is normal; compare with the old
   kernel).

## Tracing quick start (tmpfs only, never on the SD)

```sh
mount -t debugfs none /sys/kernel/debug
cd /sys/kernel/debug/tracing
echo 'p:fcr de_path_fcr_set' >> kprobe_events          # every DE "go" (the underflow check runs here)
echo 'p:pan owlfb_pan_display' >> kprobe_events        # every flip
echo 1 > events/kprobes/enable
echo 1 > events/sched/sched_switch/enable
echo 1 > events/sched/sched_wakeup/enable
echo 1 > events/irq/enable
echo 1 > events/owlfb-trace-events/enable              # dc_in / dc_to_de / dc_out
echo 1 > tracing_on; sleep 60; echo 0 > tracing_on
cat trace > /tmp/trace.txt                             # then adb pull and parse on the PC
```

`echo wakeup > current_tracer` / `echo preemptoff > current_tracer` report the worst wake-up and preempt-off
latencies (in `tracing_max_latency`): use them for "the kernel deprioritises the game".
